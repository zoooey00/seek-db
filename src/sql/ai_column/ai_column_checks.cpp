/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/ai_column/ai_column.h"
#include "sql/resolver/ob_resolver_define.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/resolver/dml/ob_insert_stmt.h"
#include "sql/resolver/dml/ob_update_stmt.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/schema/ob_trigger_info.h"
#include <set>

namespace oceanbase { namespace sql { namespace ai_column {
using namespace common;
using namespace share::schema;
bool strict_mode(ObSQLSessionInfo *session) {
  bool enabled=false;return session&&!session->is_inner()&&session->is_user_session()
      &&session->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ,enabled)==OB_SUCCESS&&enabled;
}
bool is_retry_node(const ParseNode &node) {
  return node.type_==T_ALTER_TABLE&&node.num_child_==4&&node.children_[1]
      &&node.children_[1]->type_==T_INVALID&&node.children_[1]->value_==PARSE_MARKER;
}
int check_write(ObResolverParams &params,const ObDMLStmt &stmt) {
  if(!params.session_info_||!params.session_info_->is_user_session())return OB_SUCCESS;
  if(!params.schema_checker_)return OB_NOT_INIT;
  auto check=[&](const ObColumnRefRawExpr *expr)->int {
    if(!expr)return OB_SUCCESS;
    const auto *table=stmt.get_table_item_by_id(expr->get_table_id());if(!table)return OB_SUCCESS;
    uint64_t column_id=expr->get_column_id();
    if(!table->is_basic_table()){
      const auto *item=stmt.get_column_item_by_id(expr->get_table_id(),column_id);
      if(!item||item->base_cid_==OB_INVALID_ID)return OB_SUCCESS;
      column_id=item->base_cid_;
      while(table->view_base_item_)table=table->view_base_item_;
      if(!table->is_basic_table())return OB_SUCCESS;
    }
    const ObTableSchema *schema=nullptr;int ret=params.schema_checker_->get_table_schema(table->ref_id_,schema);if(ret!=OB_SUCCESS)return ret;
    const auto *column=schema?schema->get_column_schema(column_id):nullptr;
    if(column&&is_owned(*column)){LOG_USER_ERROR(OB_NOT_SUPPORTED,"Direct assignment to AI result or state columns");return OB_NOT_SUPPORTED;}return OB_SUCCESS;
  };
  if(stmt.is_insert_stmt()){
    const auto &insert=static_cast<const ObInsertStmt&>(stmt);
    for(int64_t i=0;i<insert.get_values_desc().count();++i){int ret=check(insert.get_values_desc().at(i));if(ret!=OB_SUCCESS)return ret;}
    for(int64_t i=0;i<insert.get_table_assignments().count();++i){const auto &assignment=insert.get_table_assignments().at(i);if(!assignment.is_implicit_){int ret=check(assignment.column_expr_);if(ret!=OB_SUCCESS)return ret;}}
  }else if(stmt.is_update_stmt()){
    const auto &update=static_cast<const ObUpdateStmt&>(stmt);
    for(int64_t i=0;i<update.get_update_table_info().count();++i){auto *table=update.get_update_table_info().at(i);if(table)for(int64_t j=0;j<table->assignments_.count();++j){const auto &assignment=table->assignments_.at(j);if(!assignment.is_implicit_){int ret=check(assignment.column_expr_);if(ret!=OB_SUCCESS)return ret;}}}
  }
  return OB_SUCCESS;
}
int protected_trigger_table(ObExecContext &ctx,uint64_t trigger_id,const ObTableSchema *&table) {
  table=nullptr;
  if(!ctx.get_sql_ctx()||!ctx.get_sql_ctx()->schema_guard_)return OB_NOT_INIT;
  auto &guard=*ctx.get_sql_ctx()->schema_guard_;
  const ObTriggerInfo *trigger=nullptr;int ret=guard.get_trigger_info(trigger_id,trigger);
  if(ret!=OB_SUCCESS)return ret;if(!trigger)return OB_ERR_UNEXPECTED;
  const std::string name=string(trigger->get_trigger_name());
  const std::string owned="__seekdb_ai_"+std::to_string(trigger->get_base_object_id());
  if(name==owned+"_bi"||name==owned+"_bu")return OB_SUCCESS;
  const ObTableSchema *schema=nullptr;
  if((ret=guard.get_table_schema(trigger->get_base_object_id(),schema))!=OB_SUCCESS)return ret;
  if(!schema)return OB_ERR_UNEXPECTED;
  for(auto it=schema->column_begin();it!=schema->column_end();++it)
    if(*it&&is_result(**it)){table=schema;break;}
  return OB_SUCCESS;
}
namespace {
struct Check {std::string sql;uint64_t table_id=0,column_id=0;};
int not_ready(uint64_t table_id,uint64_t column_id) {
  OB_LOGGER.log_user_message(ObLogger::USER_ERROR,OB_AI_COLUMN_NOT_READY,
      "AI column has unfinished tasks for table_id=%lu, column_id=%lu",table_id,column_id);
  return OB_AI_COLUMN_NOT_READY;
}
void expression_columns(const ObRawExpr *expr,std::set<std::pair<uint64_t,uint64_t>>&columns,std::set<const ObRawExpr*>&visited) {
  if(!expr||!visited.insert(expr).second)return;
  if(expr->is_column_ref_expr()) {
    const auto *column=static_cast<const ObColumnRefRawExpr*>(expr);
    columns.insert({column->get_table_id(),column->get_column_id()});
  }
  for(int64_t i=0;i<expr->get_param_count();++i)expression_columns(expr->get_param_expr(i),columns,visited);
}
int collect(const ObDMLStmt &stmt,ObSchemaGetterGuard &guard,std::vector<Check>&checks,std::set<std::pair<uint64_t,uint64_t>>&seen) {
  const bool write=stmt.is_update_stmt()||stmt.is_delete_stmt();
  std::set<std::pair<uint64_t,uint64_t>>read_columns;std::set<const ObRawExpr*>visited;
  if(write) {
    const auto &conditions=stmt.get_condition_exprs();
    for(int64_t i=0;i<conditions.count();++i)expression_columns(conditions.at(i),read_columns,visited);
    const auto &orders=stmt.get_order_items();
    for(int64_t i=0;i<orders.count();++i)expression_columns(orders.at(i).expr_,read_columns,visited);
    const auto &joins=stmt.get_joined_tables();
    for(int64_t i=0;i<joins.count();++i)if(const auto *join=joins.at(i))
      for(int64_t j=0;j<join->join_conditions_.count();++j)expression_columns(join->join_conditions_.at(j),read_columns,visited);
    if(stmt.is_update_stmt()) {
      const auto &tables=static_cast<const ObUpdateStmt&>(stmt).get_update_table_info();
      for(int64_t i=0;i<tables.count();++i)if(const auto *table=tables.at(i))
        for(int64_t j=0;j<table->assignments_.count();++j) {
          const auto &assignment=table->assignments_.at(j);
          if(!assignment.is_implicit_)expression_columns(assignment.expr_,read_columns,visited);
        }
    }
  }
  for(int64_t i=0;i<stmt.get_column_items().count();++i){
    const auto &item=stmt.get_column_items().at(i);
    if(!item.expr_||!item.expr_->is_explicited_reference())continue;
    // Trigger row images contain AI results even when business SQL reads only
    // ordinary columns. For writes, inspect predicates and explicit RHS values.
    if(write&&!read_columns.count({item.table_id_,item.column_id_}))continue;
    const auto *table=stmt.get_table_item_by_id(item.table_id_);if(!table||!table->is_basic_table())continue;
    const ObTableSchema *schema=nullptr;int ret=guard.get_table_schema(table->ref_id_,schema);if(ret!=OB_SUCCESS)return ret;
    auto *column=schema?schema->get_column_schema(item.column_id_):nullptr;
    if(!column||!is_result(*column)||!seen.insert({schema->get_table_id(),column->get_column_id()}).second)continue;
    const ObDatabaseSchema *database=nullptr;ret=guard.get_database_schema(schema->get_database_id(),database);if(ret!=OB_SUCCESS)return ret;if(!database)return OB_ERR_UNEXPECTED;
    Check c;c.table_id=schema->get_table_id();c.column_id=column->get_column_id();
    std::string state=identifier(state_name(c.column_id));
    c.sql="SELECT 1 FROM "+identifier(string(database->get_database_name_str()))+"."+identifier(string(schema->get_table_name_str()))+" WHERE "+state+" IS NULL OR "+state+"<>'READY' LIMIT 1";
    checks.push_back(std::move(c));
  }
  ObSEArray<ObSelectStmt*,8> children;int ret=stmt.get_child_stmts(children);if(ret!=OB_SUCCESS)return ret;
  for(auto *child:children)if(child){ret=collect(*child,guard,checks,seen);if(ret!=OB_SUCCESS)return ret;}return OB_SUCCESS;
}
}
int check_strict(ObExecContext &ctx,const ObDMLStmt &stmt) {
  auto *session=ctx.get_my_session();if(!strict_mode(session))return OB_SUCCESS;
  if(!ctx.get_sql_ctx()||!ctx.get_sql_ctx()->schema_guard_)return OB_NOT_INIT;
  std::vector<Check>checks;std::set<std::pair<uint64_t,uint64_t>>seen;
  int ret=collect(stmt,*ctx.get_sql_ctx()->schema_guard_,checks,seen);if(ret!=OB_SUCCESS){LOG_ERROR("AI strict collect failed",K(ret));return ret;}if(checks.empty())return ret;

  if(!session->is_in_transaction()) {
    if(!ctx.get_sql_proxy())return OB_NOT_INIT;
    for(const auto &check:checks) {
      ObMySQLProxy::MySQLResult result;
      ret=ctx.get_sql_proxy()->read(result,check.sql.c_str());
      if(ret!=OB_SUCCESS){return ret;}
      if(!result.get_result())return OB_ERR_UNEXPECTED;
      ret=result.get_result()->next();
      if(ret==OB_SUCCESS){return not_ready(check.table_id,check.column_id);}
      if(ret!=OB_ITER_END)return ret;
    }
    return OB_SUCCESS;
  }
  common::sqlclient::ObISQLConnectionGuard guard;
  ret=query::ObInnerSQLConnectionAccess::create_spi_connection_with_external_session(session,guard);if(ret!=OB_SUCCESS){LOG_ERROR("AI strict connection failed",K(ret));return ret;}
  auto *conn=as_inner_sql_connection(guard.get_ptr());if(!conn){ret=OB_ERR_UNEXPECTED;LOG_ERROR("AI strict connection cast failed",K(ret));return ret;}
  for(const auto &check:checks){
    ObSQLSessionInfo::StmtSavedValue saved_session;ObIInnerSQLConnection::SavedValue saved_connection;
    if((ret=conn->begin_nested_session(saved_session,saved_connection,true))!=OB_SUCCESS){LOG_ERROR("AI strict nested begin failed",K(ret));return ret;}
    bool incomplete=false;
    {
      ObMySQLProxy::MySQLResult result;ret=conn->execute_read(ObString(check.sql.size(),check.sql.data()),result);
      if(ret==OB_SUCCESS){if(!result.get_result())ret=OB_ERR_UNEXPECTED;else {ret=result.get_result()->next();incomplete=ret==OB_SUCCESS;if(ret==OB_ITER_END)ret=OB_SUCCESS;}}
    }
    int end=conn->end_nested_session(saved_session,saved_connection);if(ret==OB_SUCCESS)ret=end;if(ret!=OB_SUCCESS){LOG_ERROR("AI strict nested query failed",K(ret),"end_ret",end);return ret;}
    if(incomplete){return not_ready(check.table_id,check.column_id);}
  }
  return OB_SUCCESS;
}
} } }
