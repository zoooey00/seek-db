/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/ai_column/ai_column.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/mysqlclient/ob_isql_connection.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "lib/thread/thread_pool.h"
#include "lib/worker.h"
#include <rapidjson/document.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <mutex>
#include <limits>
#include "lib/charset/ob_charset.h"
#include <set>
#include <unistd.h>

namespace oceanbase { namespace sql { namespace ai_column {
using namespace common;
using namespace share::schema;
namespace {
constexpr const char *META="oceanbase.__all_ai_column";
constexpr const char *TASK="oceanbase.__all_ai_column_task";
using Row=std::vector<std::string>;
using Rows=std::vector<Row>;
int write(ObISQLClient &client,const std::string &sql,int64_t *count=nullptr) {
  int64_t changed=0;int ret=client.write(sql.c_str(),changed);if(count)*count=changed;return ret;
}
int read(ObISQLClient &client,const std::string &sql,int columns,Rows &rows) {
  ObMySQLProxy::MySQLResult result;int ret=client.read(result,sql.c_str());
  if(ret!=OB_SUCCESS)return ret;
  auto *r=result.get_result();if(!r)return OB_ERR_UNEXPECTED;
  while((ret=r->next())==OB_SUCCESS){Row row;for(int i=0;i<columns;++i){ObString s;ret=r->get_varchar(i,s);if(ret==OB_ERR_NULL_VALUE){ret=OB_SUCCESS;s.reset();}if(ret!=OB_SUCCESS)return ret;row.push_back(string(s));}rows.push_back(std::move(row));}
  return ret==OB_ITER_END?OB_SUCCESS:ret;
}
std::string number(uint64_t value) {return std::to_string(value);}
int initialize_tables(ObMySQLProxy &proxy) {
  const char *ddl[]={
   "CREATE TABLE IF NOT EXISTS oceanbase.__all_ai_column(table_id BIGINT NOT NULL,column_id BIGINT NOT NULL,column_name VARCHAR(128) NOT NULL,database_name VARCHAR(128) NOT NULL,table_name VARCHAR(128) NOT NULL,normalized_expr LONGTEXT NOT NULL,definition_json LONGTEXT NOT NULL,definition_version VARCHAR(64) NOT NULL,state_column_id BIGINT NOT NULL,request_column_id BIGINT NOT NULL,result_spec_column_id BIGINT NOT NULL,max_retries INT NOT NULL,PRIMARY KEY(table_id,column_id)) COMMENT='seekdb:ai-column:metadata:v1'",
   "CREATE TABLE IF NOT EXISTS oceanbase.__all_ai_column_task(table_id BIGINT NOT NULL,column_id BIGINT NOT NULL,rowkey_hash VARCHAR(64) NOT NULL,rowkey_predicate VARCHAR(4096) NOT NULL,request_id VARCHAR(64) NOT NULL,definition_version VARCHAR(64) NOT NULL,model_input LONGTEXT,model_config LONGTEXT,state INT NOT NULL DEFAULT 1,retry_count INT NOT NULL DEFAULT 0,eligible_ts DATETIME(6) NOT NULL,claim_token VARCHAR(128),lease_until DATETIME(6),result_sql LONGTEXT,last_error VARCHAR(1024),PRIMARY KEY(table_id,column_id,rowkey_hash),KEY eligible_tasks(state,eligible_ts),KEY request_group(table_id,request_id)) COMMENT='seekdb:ai-column:tasks:v1'"};
  for(const char *sql:ddl){int ret=write(proxy,sql);if(ret!=OB_SUCCESS)return ret;}return OB_SUCCESS;
}
std::string rowkey(const ObTableSchema &schema,const char *scope) {
  if(schema.get_column_schema("__seekdb_ai_row_key")) {
    const std::string name=identifier("__seekdb_ai_row_key");
    return "CONCAT("+literal(name+" = ")+",QUOTE("+std::string(scope)+"."+name+"))";
  }
  std::string result="CONCAT(";const auto &key=schema.get_rowkey_info();
  for(int64_t i=0;i<key.get_size();++i){const auto *info=key.get_column(i);const auto *col=info?schema.get_column_schema(info->column_id_):nullptr;if(!col)return "";
    if(i)result+=","+literal(" AND ")+",";
    std::string name=string(col->get_column_name_str());result+=literal(identifier(name)+" = ")+",";
    std::string value=std::string(scope)+"."+identifier(name);
    result+=ob_is_numeric_type(col->get_data_type())?"CAST("+value+" AS CHAR)":"QUOTE("+value+")";
  }
  return result+")";
}
std::string scoped_prompt(const Definition &d) {
  // The normalized expression quotes every dependency; replace only identifier
  // tokens outside literals. Hex string literals cannot contain a backtick.
  std::string text=d.prompt;
  for(const auto &name:d.dependencies){std::string key=identifier(name),replacement="NEW."+key;size_t at=0;
    while((at=text.find(key,at))!=std::string::npos){text.replace(at,key.size(),replacement);at+=replacement.size();}}
  return text;
}
std::string changed(const ObTableSchema &schema,const Definition &d) {
  std::set<std::string> names(d.dependencies.begin(),d.dependencies.end());const auto &key=schema.get_rowkey_info();
  for(int64_t i=0;i<key.get_size();++i){auto *c=schema.get_column_schema(key.get_column(i)->column_id_);if(c)names.insert(string(c->get_column_name_str()));}
  std::string out="FALSE";for(const auto &name:names){std::string q=identifier(name);out+=" OR NOT(CAST(NEW."+q+" AS BINARY) <=> CAST(OLD."+q+" AS BINARY))";}return "("+out+")";
}
int execute_ddl(common::sqlclient::ObISQLConnection &conn,const std::string &sql) {
  int64_t affected=0;int ret=conn.execute_write(ObString(sql.size(),sql.data()),affected);
  if(ret!=OB_SUCCESS)LOG_WARN("AI column internal DDL failed",K(ret),"sql",sql.c_str());return ret;
}
int triggers(ObMySQLProxy &proxy,const ObTableSchema &schema,const std::string &db,const std::string &table,const std::vector<Definition>&defs) {
  common::sqlclient::ObISQLConnectionGuard guard;int ret=create_inner_sql_connection_for_proxy(false,0,guard);if(ret!=OB_SUCCESS)return ret;
  auto *conn=guard.get_ptr();if(!conn)return OB_ERR_UNEXPECTED;
  if((ret=execute_ddl(*conn,"USE "+identifier(db)))!=OB_SUCCESS)return ret;
  const std::string qualified=identifier(db)+"."+identifier(table),id=number(schema.get_table_id());
  const char *kinds[]={"bi","ai","bu","au","ad"};const char *timing[]={"BEFORE INSERT","AFTER INSERT","BEFORE UPDATE","AFTER UPDATE","AFTER DELETE"};
  std::string new_key=rowkey(schema,"NEW"),old_key=rowkey(schema,"OLD");if(new_key.empty())return OB_NOT_SUPPORTED;
  for(int k=0;k<5;++k){std::string trigger=identifier(db)+"."+identifier("__seekdb_ai_"+id+"_"+kinds[k]);
    Rows existing;
    if((ret=read(proxy,"SELECT trigger_name FROM information_schema.triggers WHERE trigger_schema="+literal(db)+" AND trigger_name="+literal("__seekdb_ai_"+id+"_"+kinds[k]),1,existing))!=OB_SUCCESS)return ret;
    if(!existing.empty()&&(ret=execute_ddl(*conn,"DROP TRIGGER "+trigger))!=OB_SUCCESS)return ret;
    std::string sql="CREATE TRIGGER "+trigger+" "+timing[k]+" ON "+qualified+" FOR EACH ROW BEGIN ";
    if(k==0||k==2){
      sql+="DECLARE ai_request VARCHAR(64); SET ai_request=UUID(); ";
      if(schema.get_column_schema("__seekdb_ai_row_key")) {
        std::string token=identifier("__seekdb_ai_row_key");
        sql+="IF NEW."+token+" IS NULL THEN SET NEW."+token+"=UUID(); END IF; ";
      }
      for(const auto &d:defs){std::string request=identifier(request_name(d.column_id)),state=identifier(state_name(d.column_id)),spec=identifier(spec_name(d.column_id));
        if(k==2)sql+="IF "+changed(schema,d)+" THEN ";
        sql+="SET NEW."+identifier(d.name)+"=NULL; SET NEW."+state+"='PENDING'; SET NEW."+request+"=ai_request; SET NEW."+spec+"=(SELECT definition_version FROM "+META+" WHERE table_id="+id+" AND column_id="+number(d.column_id)+"); ";
        if(k==2)sql+="END IF; ";
      }
    }else if(k==4){sql+="DELETE FROM "+std::string(TASK)+" WHERE table_id="+id+" AND rowkey_hash=SHA2("+old_key+",256); ";
    }else {
      for(const auto &d:defs){std::string col=number(d.column_id),request=identifier(request_name(d.column_id)),spec=identifier(spec_name(d.column_id));
        if(k==3)sql+="IF NOT(NEW."+request+" <=> OLD."+request+") THEN DELETE FROM "+TASK+" WHERE table_id="+id+" AND column_id="+col+" AND rowkey_hash=SHA2("+old_key+",256); ";
        sql+="INSERT INTO "+std::string(TASK)+"(table_id,column_id,rowkey_hash,rowkey_predicate,request_id,definition_version,model_input,model_config,state,retry_count,eligible_ts) VALUES("+id+","+col+",SHA2("+new_key+",256),"+new_key+",NEW."+request+",NEW."+spec+","+scoped_prompt(d)+","+literal(d.config)+",1,0,NOW(6)) ON DUPLICATE KEY UPDATE rowkey_predicate=VALUES(rowkey_predicate),request_id=VALUES(request_id),definition_version=VALUES(definition_version),model_input=VALUES(model_input),model_config=VALUES(model_config),state=1,retry_count=0,eligible_ts=NOW(6),claim_token=NULL,lease_until=NULL,result_sql=NULL,last_error=NULL; ";
        if(k==3)sql+="END IF; ";
      }
    }
    sql+="END";if((ret=execute_ddl(*conn,sql))!=OB_SUCCESS)return ret;
  }
  return OB_SUCCESS;
}
bool typed_result(const Definition &d,const ObColumnSchemaV2 &column,const std::string &text,std::string &sql) {
  if(text.size()>8*1024*1024)return false;
  if(d.type=="CHAR"){
    if(ob_is_string_type(column.get_data_type())
        && ObCharset::strlen_char(CS_TYPE_UTF8MB4_BIN,text.data(),text.size())>column.get_data_length())return false;
    sql=literal(text);return true;
  }
  rapidjson::Document j;j.Parse(text.data(),text.size());if(j.HasParseError())return false;
  if(d.embedding){if(!j.IsArray()||j.Size()!=static_cast<unsigned>(d.dimension))return false;for(const auto &v:j.GetArray())if(!v.IsNumber()||!std::isfinite(v.GetDouble()))return false;sql=text;return true;}
  if(d.type=="JSON"){sql="CAST("+literal(text)+" AS JSON)";return true;}
  if(d.type=="SIGNED"){
    if(!j.IsInt64())return false;
    int bits=64;
    switch(column.get_data_type()){
      case ObTinyIntType:bits=8;break;case ObSmallIntType:bits=16;break;
      case ObMediumIntType:bits=24;break;case ObInt32Type:bits=32;break;default:break;
    }
    const int64_t value=j.GetInt64();
    if(bits<64&&(value<-(int64_t(1)<<(bits-1))||value>((int64_t(1)<<(bits-1))-1)))return false;
    sql=std::to_string(value);return true;
  }
  if(d.type=="DOUBLE"){
    if(!j.IsNumber()||!std::isfinite(j.GetDouble()))return false;
    if(column.get_data_type()==ObFloatType&&std::fabs(j.GetDouble())>std::numeric_limits<float>::max())return false;
    sql="CAST("+literal(text)+" AS DOUBLE)";return true;
  }
  return false;
}
class Worker final:public lib::ThreadPool {
public:
  ObMySQLProxy *proxy=nullptr;
  std::atomic<uint64_t> sequence{0};
  std::atomic<bool> metadata_ready{false};
  void run1()override {
    lib::set_thread_name("AIColumn");
    while(!has_set_stop()){
      THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time()+120*1000*1000);
      int ret=tick();if(ret!=OB_SUCCESS&&ret!=OB_TABLE_NOT_EXIST&&ret!=OB_SCHEMA_ERROR&&ret!=OB_ERR_BAD_DATABASE&&ret!=OB_TRANS_STMT_NEED_RETRY)LOG_WARN("AI column worker iteration failed",K(ret));
      usleep(100000);
    }
  }
  int tick() {
    if(!metadata_ready.load()){
      ObSchemaGetterGuard guard;const ObTableSchema *schema=nullptr;
      int ready=GCTX.schema_service_->get_runtime_schema_guard(guard);
      if(ready!=OB_SUCCESS)return OB_SUCCESS;
      ready=guard.get_table_schema(ObString("oceanbase"),ObString("__all_ai_column_task"),false,schema);
      if(ready!=OB_SUCCESS||!schema)return OB_SUCCESS;
      metadata_ready.store(true);
    }
    Rows candidates;
    // Reads use a separate internal connection: uncommitted business tasks are
    // invisible. A leased CAS gives each attempt one durable owner.
    int ret=read(*proxy,"SELECT CAST(table_id AS CHAR),CAST(column_id AS CHAR),rowkey_hash,request_id FROM "+std::string(TASK)+" WHERE ((state IN(1,3) AND eligible_ts<=NOW(6)) OR (state=2 AND lease_until<NOW(6))) ORDER BY eligible_ts LIMIT 1",4,candidates);
    if(ret!=OB_SUCCESS)return ret;
    if(candidates.empty())return publish_one();
    const Row &key=candidates[0];std::string where="table_id="+key[0]+" AND column_id="+key[1]+" AND rowkey_hash="+literal(key[2])+" AND request_id="+literal(key[3]);
    std::string token=std::to_string(getpid())+":"+std::to_string(ObTimeUtility::current_time())+":"+std::to_string(sequence.fetch_add(1));int64_t claimed=0;
    ret=write(*proxy,"UPDATE "+std::string(TASK)+" SET state=2,claim_token="+literal(token)+",lease_until=DATE_ADD(NOW(6),INTERVAL 120 SECOND) WHERE "+where+" AND ((state IN(1,3) AND eligible_ts<=NOW(6)) OR (state=2 AND lease_until<NOW(6)))",&claimed);
    if(ret!=OB_SUCCESS||claimed!=1)return ret;
    Rows tasks;
    ret=read(*proxy,"SELECT t.model_input,t.model_config,CAST(t.retry_count AS CHAR),a.definition_json,a.definition_version,CAST(t.model_input IS NULL AS CHAR) FROM "+std::string(TASK)+" t JOIN "+META+" a ON t.table_id=a.table_id AND t.column_id=a.column_id AND t.definition_version=a.definition_version WHERE t.table_id="+key[0]+" AND t.column_id="+key[1]+" AND t.rowkey_hash="+literal(key[2])+" AND t.request_id="+literal(key[3])+" AND claim_token="+literal(token),6,tasks);
    if(ret!=OB_SUCCESS)return ret;
    if(tasks.empty())return write(*proxy,"DELETE FROM "+std::string(TASK)+" WHERE "+where+" AND claim_token="+literal(token));
    const Row task=tasks[0];Definition d;
    if(!decode(ObString(task[3].size(),task[3].data()),d))return OB_INVALID_ARGUMENT;
    const bool null_input=task[5]=="1";
    const std::string input=null_input?"NULL":literal(task[0]);
    Rows result;std::string query;
    if(d.embedding)query="SELECT CAST(AI_EMBED("+literal(d.model)+","+input+","+std::to_string(d.dimension)+") AS CHAR)";
    else {
      // Existing AI_COMPLETE serializes provider messages; configuration adds a
      // type contract without changing the original user input.
      std::string config=d.config.empty()?"JSON_OBJECT()":"CAST("+literal(d.config)+" AS JSON)";
      query="SELECT AI_COMPLETE("+literal(d.model)+","+input+",JSON_SET("+config+",'$.__seekdb_type_prompt',"+literal(d.hint)+"))";
    }
    int model_ret=task[0].size()>8*1024*1024?OB_SIZE_OVERFLOW:read(*proxy,query,1,result);std::string value;
    ObSchemaGetterGuard result_guard;const ObTableSchema *result_table=nullptr;
    int schema_ret=GCTX.schema_service_->get_runtime_schema_guard(result_guard);
    if(schema_ret==OB_SUCCESS)schema_ret=result_guard.get_table_schema(strtoull(key[0].c_str(),nullptr,10),result_table);
    const ObColumnSchemaV2 *result_column=result_table?result_table->get_column_schema(strtoull(key[1].c_str(),nullptr,10)):nullptr;
    bool success=model_ret==OB_SUCCESS&&schema_ret==OB_SUCCESS&&result_column&&result.size()==1&&typed_result(d,*result_column,result[0][0],value);
    if(d.embedding&&null_input&&model_ret==OB_SUCCESS&&result.size()==1){success=true;value="NULL";}
    int retries=atoi(task[2].c_str());
    std::string change;
    if(success)change="state=4,result_sql="+literal(value)+",last_error=NULL";
    else if(retries<d.max_retries)change="state=3,retry_count=retry_count+1,eligible_ts=DATE_ADD(NOW(6),INTERVAL 1 SECOND),last_error="+literal("AI call or type validation failed: "+std::to_string(model_ret));
    else change="state=5,result_sql=NULL,last_error="+literal("AI call or type validation failed: "+std::to_string(model_ret));
    ret=write(*proxy,"UPDATE "+std::string(TASK)+" SET "+change+",claim_token=NULL,lease_until=NULL WHERE "+where+" AND claim_token="+literal(token)+" AND definition_version="+literal(task[4]));
    return ret==OB_SUCCESS?publish_one():ret;
  }
  int publish_one() {
    Rows groups;int ret=read(*proxy,"SELECT CAST(table_id AS CHAR),request_id FROM "+std::string(TASK)+" WHERE eligible_ts<=NOW(6) GROUP BY table_id,request_id HAVING MIN(state)>=4 ORDER BY MIN(eligible_ts),table_id LIMIT 1",2,groups);
    if(ret!=OB_SUCCESS||groups.empty())return ret;
    ObSchemaGetterGuard schema_guard;const ObTableSchema *current=nullptr;
    if((ret=GCTX.schema_service_->get_runtime_schema_guard(schema_guard))!=OB_SUCCESS)return ret;
    if((ret=schema_guard.get_table_schema(strtoull(groups[0][0].c_str(),nullptr,10),current))!=OB_SUCCESS)return ret;
    if(!current||current->is_in_recyclebin()) {
      return write(*proxy,"DELETE FROM "+std::string(TASK)+" WHERE table_id="+groups[0][0]);
    }
    // Task ownership follows the immutable table ID across RENAME TABLE.
    // Cached metadata names can lag a committed rename; resolve the live schema.
    const ObDatabaseSchema *database=nullptr;
    if((ret=schema_guard.get_database_schema(current->get_database_id(),database))!=OB_SUCCESS)return ret;
    if(!database)return OB_ERR_UNEXPECTED;
    const std::string db_name=string(database->get_database_name_str());
    const std::string table_name=string(current->get_table_name_str());
    if((ret=write(*proxy,"UPDATE "+std::string(META)+" SET database_name="+literal(db_name)+",table_name="+literal(table_name)+" WHERE table_id="+groups[0][0]+" AND (database_name<>"+literal(db_name)+" OR table_name<>"+literal(table_name)+")"))!=OB_SUCCESS)return ret;
    ObMySQLTransaction transaction;if((ret=transaction.start(proxy))!=OB_SUCCESS)return ret;
    std::string where="t.table_id="+groups[0][0]+" AND t.request_id="+literal(groups[0][1]);Rows rows;
    ret=read(transaction,"SELECT a.database_name,a.table_name,a.column_name,CAST(t.column_id AS CHAR),t.rowkey_predicate,t.request_id,t.definition_version,CAST(t.state AS CHAR),IFNULL(t.result_sql,'NULL'),a.definition_version FROM "+std::string(TASK)+" t JOIN "+META+" a ON a.table_id=t.table_id AND a.column_id=t.column_id WHERE "+where+" ORDER BY t.column_id FOR UPDATE",10,rows);
    if(ret==OB_SUCCESS&&!rows.empty()){
      std::string update="UPDATE "+identifier(db_name)+"."+identifier(table_name)+" SET ",predicate;
      for(size_t i=0;i<rows.size();++i){const auto&r=rows[i];if(atoi(r[7].c_str())<4){ret=OB_EAGAIN;break;}
        uint64_t col=strtoull(r[3].c_str(),nullptr,10);std::string req=identifier(request_name(col)),spec=identifier(spec_name(col)),name=identifier(r[2]),state=identifier(state_name(col));
        std::string valid=req+"="+literal(r[5])+" AND "+spec+"="+literal(r[6]);if(r[6]!=r[9])valid="FALSE";
        if(i){update+=",";predicate+=" OR ";}predicate+="("+valid+")";
        update+=name+"=IF("+valid+","+(r[7]=="4"?r[8]:"NULL")+","+name+"),"+state+"=IF("+valid+","+literal(r[7]=="4"?"READY":"FAILED")+","+state+")";
      }
      if(ret==OB_SUCCESS)ret=write(transaction,update+" WHERE ("+rows[0][4]+") AND ("+predicate+")");
    }
    // A concurrent rename may invalidate these names after schema lookup.
    // Keep the completed task and retry with a fresh schema instead of deleting it.
    if(ret==OB_SUCCESS)ret=write(transaction,"DELETE FROM "+std::string(TASK)+" WHERE table_id="+groups[0][0]+" AND request_id="+literal(groups[0][1]));
    std::string error="Result publication failed: "+std::to_string(ret);
    if(ret!=OB_SUCCESS) {
      auto *session=query::ObInnerSQLConnectionAccess::get_session(transaction.get_connection());
      if(session) {
        const auto &warnings=session->get_warnings_buffer();
        if(warnings.get_err_code()==ret&&warnings.get_err_msg())error+=" "+std::string(warnings.get_err_msg());
      }
      if(error.size()>900)error.resize(900);
    }
    int end=transaction.end(ret==OB_SUCCESS);
    if(ret!=OB_SUCCESS) {
      // A failing group must not monopolize every publisher thread.
      write(*proxy,"UPDATE "+std::string(TASK)+" SET eligible_ts=DATE_ADD(NOW(6),INTERVAL 1 SECOND),last_error="+literal(error)+" WHERE table_id="+groups[0][0]+" AND request_id="+literal(groups[0][1]));
    }
    return ret==OB_SUCCESS?end:ret;
  }
};
Worker worker;std::mutex lifecycle;bool started=false;
}
int start_worker(ObMySQLProxy *proxy) {
  std::lock_guard<std::mutex> lock(lifecycle);if(started)return OB_SUCCESS;if(!proxy)return OB_INVALID_ARGUMENT;
  worker.proxy=proxy;worker.set_thread_count(4);int ret=worker.start();started=ret==OB_SUCCESS;return ret;
}
void stop_worker() {
  std::lock_guard<std::mutex> lock(lifecycle);if(started){worker.stop();worker.wait();started=false;}
}
int install(ObExecContext &ctx,const std::string &database,const std::string &table) {
  if(!ctx.get_sql_proxy()||!GCTX.schema_service_)return OB_NOT_INIT;
  int ret=OB_SUCCESS;ObSchemaGetterGuard guard;const ObTableSchema *schema=nullptr;
  if((ret=GCTX.schema_service_->get_runtime_schema_guard(guard))!=OB_SUCCESS)return ret;
  if((ret=guard.get_table_schema(ObString(database.size(),database.data()),ObString(table.size(),table.data()),false,schema))!=OB_SUCCESS)return ret;
  if(!schema)return OB_TABLE_NOT_EXIST;
  std::vector<Definition> defs;
  for(auto it=schema->column_begin();it!=schema->column_end();++it){Definition d;if(*it&&decode((*it)->get_comment_str(),d)){
    d.column_id=(*it)->get_column_id();defs.push_back(d);}}
  if(defs.empty())return OB_SUCCESS;
  if((ret=initialize_tables(*ctx.get_sql_proxy()))!=OB_SUCCESS)return ret;
  for(auto &d:defs){auto *state=schema->get_column_schema(state_name(d.column_id).c_str()),*request=schema->get_column_schema(request_name(d.column_id).c_str()),*spec=schema->get_column_schema(spec_name(d.column_id).c_str());
    auto *column=schema->get_column_schema(d.column_id);if(!state||!request||!spec||!column)return OB_ERR_UNEXPECTED;
    std::string json=string(column->get_comment_str());
    std::string sql="INSERT INTO "+std::string(META)+"(table_id,column_id,column_name,database_name,table_name,normalized_expr,definition_json,definition_version,state_column_id,request_column_id,result_spec_column_id,max_retries) VALUES("+number(schema->get_table_id())+","+number(d.column_id)+","+literal(d.name)+","+literal(database)+","+literal(table)+","+literal(d.expression)+","+literal(json)+",SHA2("+literal(json)+",256),"+number(state->get_column_id())+","+number(request->get_column_id())+","+number(spec->get_column_id())+","+std::to_string(d.max_retries)+") ON DUPLICATE KEY UPDATE normalized_expr=VALUES(normalized_expr),definition_json=VALUES(definition_json),definition_version=VALUES(definition_version),max_retries=VALUES(max_retries),database_name=VALUES(database_name),table_name=VALUES(table_name)";
    if((ret=write(*ctx.get_sql_proxy(),sql))!=OB_SUCCESS)return ret;
  }
  if((ret=triggers(*ctx.get_sql_proxy(),*schema,database,table,defs))!=OB_SUCCESS)return ret;
  for(const auto &d:defs){
    std::string id=number(d.column_id),spec=identifier(spec_name(d.column_id));
    std::string version="(SELECT definition_version FROM "+std::string(META)+" WHERE table_id="+number(schema->get_table_id())+" AND column_id="+id+")";
    std::string update="UPDATE "+identifier(database)+"."+identifier(table)+" SET "+identifier(d.name)+"=NULL,"+identifier(state_name(d.column_id))+"='PENDING',"+identifier(request_name(d.column_id))+"=UUID(),"+spec+"="+version+" WHERE "+spec+" IS NULL OR "+spec+"<>"+version;
    if((ret=write(*ctx.get_sql_proxy(),update))!=OB_SUCCESS)return ret;
  }
  worker.metadata_ready.store(true);
  return start_worker(ctx.get_sql_proxy());
}
} } }
