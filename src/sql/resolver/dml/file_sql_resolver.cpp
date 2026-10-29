/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/file_sql_resolver.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include <set>

namespace oceanbase { namespace sql {
using namespace common;
namespace {
std::string node_text(const ParseNode *node)
{ return node && node->str_value_ ? std::string(node->str_value_,node->str_len_) : std::string(); }
std::string lowercase(std::string s)
{ for(char &c:s)if(c>='A'&&c<='Z')c+='a'-'A';return s; }
bool literal(const ParseNode *node,std::string &value,const ParamStore *params=nullptr)
{
  if(node==nullptr)return false;
  switch(node->type_){
    case T_QUESTIONMARK: {
      if(params==nullptr||node->value_<0||node->value_>=params->count())return false;
      const ObObj &v=params->at(node->value_);
      if(v.is_string_type()){auto s=v.get_string();value.assign(s.ptr(),s.length());return true;}
      if(v.is_integer_type()){value=std::to_string(v.get_int());return true;}
      return false;
    }
    case T_VARCHAR:case T_CHAR:value=node_text(node);return true;
    case T_INT:value=std::to_string(node->value_);return true;
    case T_BOOL:value=node->value_?"true":"false";return true;
    case T_OP_NEG:if(node->num_child_==1&&literal(node->children_[0],value,params)){value="-"+value;return true;}return false;
    default:return false;
  }
}
std::string option_name(const ParseNode *node)
{
  if(node&&node->type_==T_COLUMN_REF&&node->num_child_==3&&node->children_[0]==nullptr&&node->children_[1]==nullptr)
    return lowercase(node_text(node->children_[2]));
  return lowercase(node_text(node));
}
}

int ObDMLResolver::resolve_file_table_item(const ParseNode &node,TableItem *&table)
{
  int ret=OB_SUCCESS;bool enabled=false;ObString secure;
  file_sql::Options options;file_sql::Spec spec;std::string error,path,encoded;
  std::string name=lowercase(node_text(&node));
  if(name=="read_csv")options.format="csv";
  else if(name=="read_json"||name=="read_ndjson")options.format="json";
  else if(name=="read_parquet")options.format="parquet";
  else if(name=="glob")options.format="glob";
  else {ret=OB_NOT_SUPPORTED;LOG_USER_ERROR(OB_NOT_SUPPORTED,"unknown file table function");}
  if(OB_SUCC(ret)&&OB_FAIL(session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL,enabled))){}
  else if(OB_SUCC(ret)&&!enabled){ret=OB_NOT_SUPPORTED;LOG_USER_ERROR(OB_NOT_SUPPORTED,"file SQL; set enable_file_sql=ON first");}
  if(OB_SUCC(ret)&&OB_FAIL(session_info_->get_secure_file_priv(secure))){}
  // The inferred column layout depends on file paths and options. Pin every
  // parameter of this plan so a normalized query cannot reuse another file's
  // schema or contents (also covers automatically parameterized literals).
  if(OB_SUCC(ret)&&params_.param_list_!=nullptr&&params_.param_list_->count()>0){
    ObPCConstParamInfo constraints;
    for(int64_t i=0;OB_SUCC(ret)&&i<params_.param_list_->count();++i){
      if(OB_FAIL(constraints.const_idx_.push_back(i))){}
      else if(OB_FAIL(constraints.const_params_.push_back(params_.param_list_->at(i)))){}
    }
    if(OB_SUCC(ret))ret=params_.query_ctx_->all_plan_const_param_constraints_.push_back(constraints);
  }
  const ParseNode *args=node.children_[0];std::set<std::string> seen;
  if(OB_SUCC(ret)&&(args==nullptr||args->num_child_<1||!literal(args->children_[0],path,params_.param_list_))){ret=OB_INVALID_ARGUMENT;error="File path must be a string literal";}
  for(int i=1;OB_SUCC(ret)&&i<args->num_child_;++i){
    const ParseNode *arg=args->children_[i];std::string key,value;
    if(arg==nullptr||arg->type_!=T_OP_EQ||arg->num_child_!=2){
      LOG_USER_ERROR(OB_ERR_PARSE_SQL, ob_errpkt_strerror(OB_ERR_PARSER_SYNTAX),
                     0, "", 1);
      return OB_ERR_PARSE_SQL;
    }
    if(!literal(arg->children_[1],value,params_.param_list_)){
      ret=OB_INVALID_ARGUMENT;error="File options must be named literal parameters";break;
    }
    key=option_name(arg->children_[0]);
    if(!seen.insert(key).second){ret=OB_INVALID_ARGUMENT;error="Duplicate file option: "+key;}
    else if(key=="sample_size"){
      if(value!="-1"){ret=OB_NOT_SUPPORTED;error="sample_size supports only -1 (full schema inference)";}
    }else if(key=="format"&&options.format=="json"){
      if(value!="newline_delimited"&&value!="ndjson"&&value!="jsonl"){ret=OB_NOT_SUPPORTED;error="Only newline-delimited flat JSON objects are supported";}
    }else if(key=="header"&&options.format=="csv"){
      if(value=="true"||value=="1")options.header=true;else if(value=="false"||value=="0")options.header=false;else {ret=OB_INVALID_ARGUMENT;error="header requires a boolean";}
    }else if((key=="delimiter"||key=="delim"||key=="sep")&&options.format=="csv"){
      if(value.size()!=1||value[0]=='\n'||value[0]=='\r'||value[0]=='\0'){ret=OB_INVALID_ARGUMENT;error="CSV delimiter must be one non-newline byte";}else options.delimiter=value[0];
    }else if((key=="quote"||key=="escape")&&options.format=="csv"){
      if(value.size()>1||(!value.empty()&&(value[0]=='\n'||value[0]=='\r'||value[0]=='\0'))){ret=OB_INVALID_ARGUMENT;error="CSV quote/escape must be empty or one non-newline byte";}
      else if(key=="quote")options.quote=value.empty()?0:value[0];else options.escape=value.empty()?0:value[0];
    }else if((key=="nullstr"||key=="null_string")&&options.format=="csv")options.nullstr=value;
    else {ret=OB_INVALID_ARGUMENT;error="Unknown file option: "+key;}
  }
  if(OB_SUCC(ret)&&options.delimiter==options.quote){ret=OB_INVALID_ARGUMENT;error="CSV delimiter and quote must differ";}
  if(OB_SUCC(ret)&&!file_sql::infer(path,std::string(secure.ptr(),secure.length()),options,spec,error))ret=OB_INVALID_ARGUMENT;
  if(OB_SUCC(ret)&&!file_sql::encode(spec,encoded,error))ret=OB_INVALID_ARGUMENT;
  if(OB_FAIL(ret)&&!error.empty()){
    if(ret==OB_NOT_SUPPORTED)LOG_USER_ERROR(OB_NOT_SUPPORTED,error.c_str());
    else LOG_USER_ERROR(OB_INVALID_ARGUMENT,error.c_str());
  }
  ObConstRawExpr *value=nullptr;ObString stored;
  if(OB_SUCC(ret)&&OB_FAIL(params_.expr_factory_->create_raw_expr(T_VARCHAR,value))){}
  if(OB_SUCC(ret)&&OB_FAIL(ob_write_string(*allocator_,ObString(encoded.size(),encoded.data()),stored))){}
  if(OB_SUCC(ret)){
    ObObj obj;obj.set_varchar(stored);obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);value->set_value(obj);
    value->set_data_type(ObVarcharType);value->set_collation_type(CS_TYPE_UTF8MB4_BIN);
    if(OB_FAIL(value->formalize(session_info_))){}
  }
  if(OB_SUCC(ret)){
    table=get_stmt()->create_table_item(*allocator_);
    if(table==nullptr)ret=OB_ALLOCATE_MEMORY_FAILED;
    else {
      table->type_=TableItem::FUNCTION_TABLE;table->table_id_=generate_table_id();table->function_table_expr_=value;
      const ParseNode *alias=node.children_[1];
      if(alias)ret=ob_write_string(*allocator_,ObString(alias->str_len_,alias->str_value_),table->alias_name_);
      else ret=get_stmt()->generate_func_table_name(*allocator_,table->alias_name_);
      table->table_name_=table->alias_name_;
      if(OB_SUCC(ret)&&OB_FAIL(get_stmt()->add_table_item(session_info_,table))){}
      ObSEArray<ColumnItem,16> columns;
      if(OB_SUCC(ret))ret=resolve_file_table_columns(*table,columns);
    }
  }
  return ret;
}

int ObDMLResolver::resolve_file_table_columns(const TableItem &table,ObIArray<ColumnItem>&columns)
{
  int ret=OB_SUCCESS;file_sql::Spec spec;std::string error;
  const ObObj &value=static_cast<const ObConstRawExpr*>(table.function_table_expr_)->get_value();
  ObString encoded=value.get_varchar();
  if(!file_sql::decode(std::string(encoded.ptr(),encoded.length()),spec,error))return OB_INVALID_ARGUMENT;
  for(size_t i=0;OB_SUCC(ret)&&i<spec.columns.size();++i){
    const auto &c=spec.columns[i];ColumnItem *item=get_stmt()->get_column_item(table.table_id_,OB_APP_MIN_COLUMN_ID+i);
    if(item==nullptr){
      ObObjMeta meta;ObAccuracy accuracy;accuracy.set_length(static_cast<int32_t>(std::max<int64_t>(c.max_length,1)));
      switch(c.type){
        case file_sql::Type::Bigint:meta.set_int();break;
        case file_sql::Type::Double:meta.set_double();break;
        case file_sql::Type::Boolean:meta.set_tinyint();break;
        case file_sql::Type::Date:meta.set_date();break;
        case file_sql::Type::Datetime:meta.set_datetime();accuracy.set_scale(c.max_length>20&&c.max_length<=26?c.max_length-20:0);break;
        default:meta.set_varchar();meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);meta.set_collation_level(CS_LEVEL_IMPLICIT);accuracy.set_length(std::max<int64_t>(65535,c.max_length));break;
      }
      ObString name;ret=ob_write_string(*allocator_,ObString(c.name.size(),c.name.data()),name);
      if(OB_SUCC(ret))ret=resolve_function_table_column_item(table,meta,accuracy,name,OB_APP_MIN_COLUMN_ID+i,item);
      if(OB_SUCC(ret)&&!c.nullable)item->expr_->set_result_flag(NOT_NULL_FLAG);
    }
    if(OB_SUCC(ret))ret=columns.push_back(*item);
  }
  return ret;
}
}} // namespace oceanbase::sql
