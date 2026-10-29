/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/ai_column/ai_column.h"
#include "sql/parser/parse_node.h"
#include "share/schema/ob_table_schema.h"
#include "share/ob_rpc_struct.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "lib/oblog/ob_log.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <set>

namespace oceanbase { namespace sql { namespace ai_column {
using namespace common;
using namespace share::schema;
namespace {
std::string text(const ParseNode *n) { return n&&n->str_value_?std::string(n->str_value_,n->str_len_):""; }
std::string lower(std::string s) { for(char &c:s)if(c>='A'&&c<='Z')c+='a'-'A';return s; }
std::string json(const Definition &d) {
  rapidjson::StringBuffer out;rapidjson::Writer<rapidjson::StringBuffer>w(out);
  w.StartObject();
#define AI_STRING(key) w.Key(#key);w.String(d.key.data(),d.key.size());
  AI_STRING(name) AI_STRING(model) AI_STRING(prompt) AI_STRING(config) AI_STRING(expression) AI_STRING(type) AI_STRING(hint)
#undef AI_STRING
  w.Key("embedding");w.Bool(d.embedding);w.Key("dimension");w.Int(d.dimension);
  w.Key("max_retries");w.Int(d.max_retries);w.Key("dependencies");w.StartArray();
  for(const auto &s:d.dependencies)w.String(s.data(),s.size());w.EndArray();w.EndObject();
  return std::string(COMMENT_PREFIX)+out.GetString();
}
bool walk(const ParseNode *n,Definition &d,std::string &out) {
  if(!n)return false;
  switch(n->type_){
    case T_VARCHAR:case T_CHAR:out=literal(text(n));return true;
    case T_NULL:out="NULL";return true;
    case T_INT:out=std::to_string(n->value_);return true;
    case T_BOOL:out=n->value_?"TRUE":"FALSE";return true;
    case T_COLUMN_REF: {
      if(n->num_child_!=3||n->children_[0]||n->children_[1]||!n->children_[2])return false;
      std::string name=text(n->children_[2]);
      if(std::find(d.dependencies.begin(),d.dependencies.end(),name)==d.dependencies.end())d.dependencies.push_back(name);
      out=identifier(name);return true;
    }
    case T_FUN_SYS: {
      if(n->num_child_!=2||!n->children_[0]||!n->children_[1])return false;
      std::string fn=lower(text(n->children_[0]));
      // Side effects, session variables, nested AI calls and subqueries are excluded.
      static const std::set<std::string> allowed={"concat","concat_ws","coalesce","ifnull","lower","upper","trim","ltrim","rtrim","replace","substring","substr","left","right","length","char_length","abs","round","if","nullif","json_object","json_array","json_extract","json_unquote"};
      if(!allowed.count(fn))return false;
      out=fn+"(";const ParseNode *args=n->children_[1];
      for(int i=0;i<args->num_child_;++i){std::string s;if(!walk(args->children_[i],d,s))return false;if(i)out+=",";out+=s;}
      out+=")";return true;
    }
    default: {
      const char *op=nullptr;
      switch(n->type_){case T_OP_ADD:op="+";break;case T_OP_MINUS:op="-";break;case T_OP_MUL:op="*";break;case T_OP_DIV:op="/";break;default:break;}
      if(!op||n->num_child_!=2)return false;
      std::string a,b;if(!walk(n->children_[0],d,a)||!walk(n->children_[1],d,b))return false;
      out="("+a+op+b+")";return true;
    }
  }
}
int invalid(const char *message) { int ret=OB_NOT_SUPPORTED; LOG_WARN("Invalid AI column definition","reason",message);return ret; }
}
std::string string(const ObString &s) { return s.ptr()?std::string(s.ptr(),s.length()):""; }
std::string identifier(const std::string &s) { std::string out="`";for(char c:s){out+=c;if(c=='`')out+=c;}return out+"`"; }
std::string literal(const std::string &s) { static const char hex[]="0123456789abcdef";std::string out="CONVERT(X'";for(unsigned char c:s){out+=hex[c>>4];out+=hex[c&15];}return out+"' USING utf8mb4)"; }
std::string state_name(uint64_t id) {return std::string(HIDDEN_PREFIX)+"state_"+std::to_string(id);}
std::string request_name(uint64_t id) {return std::string(HIDDEN_PREFIX)+"request_"+std::to_string(id);}
std::string spec_name(uint64_t id) {return std::string(HIDDEN_PREFIX)+"spec_"+std::to_string(id);}
bool decode(const ObString &comment,Definition &d) {
  if(!comment.prefix_match(COMMENT_PREFIX))return false;
  std::string s=string(comment).substr(strlen(COMMENT_PREFIX));rapidjson::Document j;j.Parse(s.data(),s.size());
  if(j.HasParseError()||!j.IsObject())return false;
#define AI_READ(key) if(!j.HasMember(#key)||!j[#key].IsString())return false;d.key=j[#key].GetString();
  AI_READ(name) AI_READ(model) AI_READ(prompt) AI_READ(config) AI_READ(expression) AI_READ(type) AI_READ(hint)
#undef AI_READ
  if(!j.HasMember("embedding")||!j["embedding"].IsBool()||!j.HasMember("max_retries")||!j["max_retries"].IsInt()||!j.HasMember("dimension")||!j["dimension"].IsInt()||!j.HasMember("dependencies")||!j["dependencies"].IsArray())return false;
  d.embedding=j["embedding"].GetBool();d.dimension=j["dimension"].GetInt();d.max_retries=j["max_retries"].GetInt();d.dependencies.clear();
  for(const auto &v:j["dependencies"].GetArray()){if(!v.IsString())return false;d.dependencies.emplace_back(v.GetString());}return true;
}
bool is_result(const ObColumnSchemaV2 &c) { return c.get_comment_str().prefix_match(COMMENT_PREFIX); }
bool is_owned(const ObColumnSchemaV2 &c) { return is_result(c)||c.get_column_name_str().prefix_match(HIDDEN_PREFIX); }
bool is_trigger_column(const ObTableSchema &schema,const ObColumnSchemaV2 &column) {
  if(!column.is_hidden()||column.get_column_name_str().prefix_match(HIDDEN_PREFIX))return true;
  if(column.is_rowkey_column()) {
    for(auto it=schema.column_begin();it!=schema.column_end();++it)if(*it&&is_result(**it))return true;
  }
  return false;
}
int resolve_definition(ObColumnSchemaV2 &column,const ParseNode &attribute) {
  Definition d;d.name=string(column.get_column_name_str());const ParseNode *expr=attribute.children_[0];
  if(!expr||expr->type_!=T_FUN_SYS||expr->num_child_!=2||!expr->children_[0]||!expr->children_[1])return invalid("AI COLUMN requires AI_COMPLETE or AI_EMBED");
  std::string fn=lower(text(expr->children_[0]));d.embedding=fn=="ai_embed";
  const ParseNode *args=expr->children_[1];
  if((fn!="ai_complete"&&fn!="ai_embed")||args->num_child_<2||args->num_child_>3)return invalid("AI COLUMN function arguments");
  const ParseNode *model=args->children_[0];if(!model||(model->type_!=T_VARCHAR&&model->type_!=T_CHAR)||text(model).empty())return invalid("AI COLUMN model must be a nonempty string literal");
  d.model=text(model);
  {
    ObArenaAllocator allocator("AIColumnDef");ObAIFuncExprInfo *info=nullptr;
    int ret=ObAIFuncUtils::get_ai_func_info(allocator,ObString(d.model.size(),d.model.data()),info);
    if(ret!=OB_SUCCESS)return ret;
    if(!info||(d.embedding?!ObAIFuncUtils::is_dense_embedding_type(info):!ObAIFuncUtils::is_completion_type(info)))return invalid("AI COLUMN model type does not match its function");
  }
  if(!walk(args->children_[1],d,d.prompt))return invalid("AI COLUMN requires a deterministic scalar input expression");
  if(d.embedding){
    const auto &info=column.get_extended_type_info();
    if(!column.is_collection()||info.count()==0)return invalid("AI_EMBED requires a VECTOR result column");
    std::string type=string(info.at(0));auto at=type.find('(');if(at==std::string::npos)return invalid("AI COLUMN vector dimension");
    d.dimension=atoi(type.c_str()+at+1);if(d.dimension<1||d.dimension>16384)return invalid("AI COLUMN vector dimension");
    if(args->num_child_==3&&(!args->children_[2]||args->children_[2]->type_!=T_INT||args->children_[2]->value_!=d.dimension)){
      LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<unsigned>(d.dimension), static_cast<unsigned>(args->children_[2]?args->children_[2]->value_:0));return OB_ERR_INVALID_VECTOR_DIM;
    }
    d.type="VECTOR("+std::to_string(d.dimension)+")";
  }else {
    switch(column.get_data_type()){
      case ObJsonType:d.type="JSON";d.hint="Return exactly one valid JSON value. Do not include Markdown fences or explanatory text.";break;
      case ObTinyIntType:case ObSmallIntType:case ObMediumIntType:case ObInt32Type:case ObIntType:d.type="SIGNED";d.hint="Return exactly one base-10 integer. Do not include explanatory text.";break;
      case ObFloatType:case ObDoubleType:case ObNumberType:d.type="DOUBLE";d.hint="Return exactly one finite JSON number. Do not include explanatory text.";break;
      case ObVarcharType:case ObCharType:case ObTinyTextType:case ObTextType:case ObMediumTextType:case ObLongTextType:d.type="CHAR";break;
      default:return invalid("AI_COMPLETE result type");
    }
    if(args->num_child_==3){const ParseNode *config=args->children_[2];if(!config||config->type_!=T_VARCHAR)return invalid("AI COLUMN config must be a constant JSON object");rapidjson::Document j;j.Parse(text(config).c_str());if(j.HasParseError()||!j.IsObject())return invalid("AI COLUMN config must be a constant JSON object");d.config=text(config);}
  }
  const ParseNode *opts=attribute.children_[1];std::set<std::string>seen;
  for(int i=0;opts&&i<opts->num_child_;++i){const auto *opt=opts->children_[i];if(!opt||opt->type_!=T_OP_EQ||opt->num_child_!=2)return invalid("AI COLUMN options must be named literals");
    const auto *key=opt->children_[0],*v=opt->children_[1];if(!key||!v)return invalid("AI COLUMN option");
    std::string name=lower(key->type_==T_COLUMN_REF?text(key->children_[2]):text(key));if(!seen.insert(name).second)return invalid("Duplicate AI COLUMN option");
    if(name=="sync_mode"){if(v->type_!=T_VARCHAR||lower(text(v))!="async")return invalid("AI COLUMN supports only sync_mode='async'");}
    else if(name=="max_retries"){if(v->type_!=T_INT||v->value_<0||v->value_>10)return invalid("AI COLUMN max_retries must be between 0 and 10");d.max_retries=v->value_;}
    else return invalid("Unknown AI COLUMN option");
  }
  d.expression=(d.embedding?"AI_EMBED(":"AI_COMPLETE(")+literal(d.model)+","+d.prompt+")";
  if(!d.hint.empty())d.expression+=" /* "+d.hint+" */";
  if(column.is_rowkey_column()||column.is_generated_column())return invalid("AI COLUMN cannot be a primary key or generated column");
  std::string encoded=json(d);if(encoded.size()>8192)return invalid("AI COLUMN definition exceeds 8 KiB");
  column.set_nullable(true);return column.set_comment(ObString(encoded.size(),encoded.data()));
}
int prepare_schema(ObTableSchema &schema,const ObTableSchema *original) {
  int ret=OB_SUCCESS;std::vector<Definition> definitions;
  bool has_ai=false;
  for(auto it=schema.column_begin();it!=schema.column_end();++it)if(*it&&is_result(**it))has_ai=true;
  if(!has_ai)return OB_SUCCESS;
  uint64_t next_id=original?original->get_max_used_column_id():schema.get_max_used_column_id();
  for(auto it=schema.column_begin();it!=schema.column_end();++it)if(*it&&(*it)->get_column_id()!=OB_INVALID_ID)next_id=std::max(next_id,(*it)->get_column_id());
  for(auto it=schema.column_begin();it!=schema.column_end();++it)if(*it&&(*it)->get_column_id()==OB_INVALID_ID)(*it)->set_column_id(++next_id);
  for(auto it=schema.column_begin();it!=schema.column_end();++it){if(!*it)return OB_ERR_UNEXPECTED;Definition d;if(decode((*it)->get_comment_str(),d)){if((*it)->is_rowkey_column())return invalid("AI COLUMN cannot be a primary key");d.column_id=(*it)->get_column_id();definitions.push_back(d);}}
  if(definitions.empty())return ret;
  uint64_t max_id=original?original->get_max_used_column_id():schema.get_max_used_column_id();
  for(auto it=schema.column_begin();it!=schema.column_end();++it)max_id=std::max(max_id,(*it)->get_column_id());
  bool explicit_key=false;
  const ObTableSchema &key_schema=original?*original:schema;
  for(auto it=key_schema.column_begin();it!=key_schema.column_end();++it)
    if(*it&&(*it)->is_rowkey_column()&&!(*it)->is_hidden())explicit_key=true;
  const char *row_token="__seekdb_ai_row_key";
  if(!explicit_key&&!schema.get_column_schema(row_token)&&(!original||!original->get_column_schema(row_token))) {
    AlterColumnSchema col;col.set_column_id(++max_id);col.set_table_id(schema.get_table_id());
    col.set_data_type(ObVarcharType);col.set_data_length(64);col.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    col.set_charset_type(CHARSET_UTF8MB4);col.set_nullable(true);col.set_is_hidden(true);col.alter_type_=OB_DDL_ADD_COLUMN;
    if(OB_FAIL(col.set_column_name(row_token)))return ret;
    if(original){if(OB_FAIL(static_cast<AlterTableSchema&>(schema).add_alter_column(col,true)))return ret;}
    else if(OB_FAIL(schema.add_column(col)))return ret;
  }
  for(auto &d:definitions){
    for(const auto &name:d.dependencies){const ObColumnSchemaV2 *dep=schema.get_column_schema(ObString(name.size(),name.data()));if(!dep&&original)dep=original->get_column_schema(ObString(name.size(),name.data()));
      if(!dep||is_owned(*dep))return invalid("AI COLUMN dependencies must reference existing ordinary columns");}
    for(int index=0;index<3;++index){std::string name=index==0?state_name(d.column_id):index==1?request_name(d.column_id):spec_name(d.column_id);
      if(schema.get_column_schema(ObString(name.size(),name.data()))||(original&&original->get_column_schema(ObString(name.size(),name.data()))))continue;
      AlterColumnSchema col;col.set_column_id(++max_id);col.set_table_id(schema.get_table_id());col.set_data_type(ObVarcharType);col.set_data_length(index==0?16:64);col.set_collation_type(CS_TYPE_UTF8MB4_BIN);col.set_charset_type(CHARSET_UTF8MB4);col.set_nullable(true);col.set_is_hidden(true);col.alter_type_=OB_DDL_ADD_COLUMN;
      if(OB_FAIL(col.set_column_name(ObString(name.size(),name.data()))))return ret;
      if(original){
        if(OB_FAIL(static_cast<AlterTableSchema&>(schema).add_alter_column(col,true)))return ret;
      }else if(OB_FAIL(schema.add_column(col)))return ret;
    }
  }
  schema.set_max_used_column_id(max_id);return ret;
}
} } }
