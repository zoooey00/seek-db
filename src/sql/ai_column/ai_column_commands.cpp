/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/ai_column/ai_column.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/parser/ob_parser.h"
#include "sql/session/ob_sql_session_info.h"
#include <cctype>

namespace oceanbase { namespace sql {
using namespace common;
namespace {
std::string node_text(const ParseNode *node) {return node&&node->str_value_?std::string(node->str_value_,node->str_len_):"";}
std::string retry_predicate(const std::string &sql,const std::string &state,bool no_backslash) {
  std::string out;size_t at=0;
  while(at<sql.size()){
    char c=sql[at];
    if(c=='\''||c=='"'||c=='`'){
      char quote=c;out+=sql[at++];
      while(at<sql.size()){
        c=sql[at++];out+=c;
        if(c=='\\'&&quote!='`'&&!no_backslash&&at<sql.size()){out+=sql[at++];continue;}
        if(c==quote){if(at<sql.size()&&sql[at]==quote){out+=sql[at++];}else break;}
      }
    }else if(std::isalpha(static_cast<unsigned char>(c))||c=='_'){
      size_t begin=at;while(at<sql.size()&&(std::isalnum(static_cast<unsigned char>(sql[at]))||sql[at]=='_'||sql[at]=='$'))++at;
      std::string token=sql.substr(begin,at-begin),lower=token;for(char &v:lower)v=std::tolower(static_cast<unsigned char>(v));
      out+=lower=="status"?state:token;
    }else {out+=c;++at;}
  }
  return out;
}
}
int ObResolver::resolve_ai_retry(const ParseNode &node,ObStmt *&stmt) {
  if(!ai_column::is_retry_node(node)||!params_.schema_checker_||!params_.session_info_)return OB_INVALID_ARGUMENT;
  const ParseNode *relation=node.children_[0],*retry=node.children_[1];
  if(!relation||relation->num_child_<2||!relation->children_[1])return OB_INVALID_ARGUMENT;
  std::string database=node_text(relation->children_[0]),table=node_text(relation->children_[1]);
  if(database.empty())database=ai_column::string(params_.session_info_->get_database_name());
  const share::schema::ObTableSchema *schema=nullptr;
  int ret=params_.schema_checker_->get_table_schema(ObString(database.size(),database.data()),ObString(table.size(),table.data()),false,schema);
  if(ret!=OB_SUCCESS)return ret;if(!schema)return OB_TABLE_NOT_EXIST;
  std::string name=node_text(retry->children_[0]);auto *column=schema->get_column_schema(ObString(name.size(),name.data()));
  if(retry->num_child_>2 && retry->children_[2]) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED,"combining AI COLUMN RETRY with another ALTER action");return OB_NOT_SUPPORTED;
  }
  if(!column||!ai_column::is_result(*column)) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED,"retrying a column that is not an AI column");return OB_NOT_SUPPORTED;
  }
  uint64_t id=column->get_column_id();
  std::string state=ai_column::identifier(ai_column::state_name(id)),request=ai_column::identifier(ai_column::request_name(id)),spec=ai_column::identifier(ai_column::spec_name(id));
  std::string sql="UPDATE "+ai_column::identifier(database)+"."+ai_column::identifier(table)+" SET "+ai_column::identifier(name)+"=NULL,"+state+"='PENDING',"+request+"=UUID(),"+spec+"=SHA2("+ai_column::literal(ai_column::string(column->get_comment_str()))+",256)";
  if(retry->children_[1]){
    bool no_backslash=(params_.session_info_->get_sql_mode()&SMO_NO_BACKSLASH_ESCAPES)!=0;
    sql+=" "+retry_predicate(node_text(retry->children_[1]),state,no_backslash);
  }
  ObString stored;if((ret=ob_write_string(*params_.allocator_,ObString(sql.size(),sql.data()),stored))!=OB_SUCCESS)return ret;
  ObParser parser(*params_.allocator_,params_.session_info_->get_sql_mode());ParseResult parsed;
  if((ret=parser.parse(stored,parsed))!=OB_SUCCESS)return ret;
  if(!parsed.result_tree_||parsed.result_tree_->num_child_!=1)return OB_ERR_UNEXPECTED;
  const uint64_t saved=params_.ai_retry_table_id_;
  params_.ai_retry_table_id_=schema->get_table_id();
  ret=resolve(IS_NOT_PREPARED_STMT,*parsed.result_tree_->children_[0],stmt);
  params_.ai_retry_table_id_=saved;
  if(ret==OB_SUCCESS && params_.query_ctx_)params_.query_ctx_->set_literal_stmt_type(stmt::T_ALTER_TABLE);
  return ret;
}
int ObResolver::resolve_ai_show(const ParseNode &node,ObStmt *&stmt) {
  if(node.num_child_!=5||!node.children_[4]||!params_.schema_checker_||!params_.session_info_)return OB_INVALID_ARGUMENT;
  const ParseNode *relation=node.children_[4];
  if(relation->num_child_<2||!relation->children_[1])return OB_INVALID_ARGUMENT;
  std::string database=node_text(relation->children_[0]),table=node_text(relation->children_[1]);
  if(database.empty())database=ai_column::string(params_.session_info_->get_database_name());
  const share::schema::ObTableSchema *schema=nullptr;
  int ret=params_.schema_checker_->get_table_schema(ObString(database.size(),database.data()),ObString(table.size(),table.data()),false,schema);
  if(ret!=OB_SUCCESS)return ret;if(!schema)return OB_TABLE_NOT_EXIST;
  std::string sql="SELECT column_name,normalized_expr,definition_json,max_retries FROM oceanbase.__all_ai_column WHERE table_id="+std::to_string(schema->get_table_id())+" ORDER BY column_id";
  ObString stored;if((ret=ob_write_string(*params_.allocator_,ObString(sql.size(),sql.data()),stored))!=OB_SUCCESS)return ret;
  ObParser parser(*params_.allocator_,params_.session_info_->get_sql_mode());ParseResult parsed;
  if((ret=parser.parse(stored,parsed))!=OB_SUCCESS)return ret;
  if(!parsed.result_tree_||parsed.result_tree_->num_child_!=1)return OB_ERR_UNEXPECTED;
  return resolve(IS_NOT_PREPARED_STMT,*parsed.result_tree_->children_[0],stmt);
}
} }
