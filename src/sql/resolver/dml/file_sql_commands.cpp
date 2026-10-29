/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/dml/file_sql_resolver.h"
#include "sql/parser/ob_parser.h"
#include "sql/session/ob_sql_session_info.h"
#include <set>

namespace oceanbase { namespace sql {
using namespace common;
namespace {
std::string text(const ObString&s){return std::string(s.ptr()?s.ptr():"",s.length());}
std::string text(const ParseNode*n){return n&&n->str_value_?std::string(n->str_value_,n->str_len_):std::string();}
std::string lower(std::string s){for(char&c:s)if(c>='A'&&c<='Z')c+='a'-'A';return s;}
// Hex literals avoid any dependence on NO_BACKSLASH_ESCAPES or connection mode.
std::string sql_text(const std::string&s){static const char hex[]="0123456789abcdef";std::string out="CONVERT(X'";for(unsigned char c:s){out+=hex[c>>4];out+=hex[c&15];}return out+"' USING utf8mb4)";}
}
int ObResolver::resolve_file_describe(const ParseNode&node,ObStmt*&stmt)
{
  if(node.num_child_!=5||node.children_[4]==nullptr||node.children_[4]->type_!=T_SELECT)return OB_INVALID_ARGUMENT;
  int ret=OB_SUCCESS;ObSelectResolver source(params_);
  if(OB_FAIL(source.resolve(*node.children_[4])))return ret;
  ObSelectStmt *select=source.get_select_stmt();if(select==nullptr)return OB_ERR_UNEXPECTED;
  bool file_table=false;for(int64_t i=0;i<select->get_table_size();++i){const auto*t=select->get_table_item(i);file_table|=t&&is_file_sql_expr(t->function_table_expr_);}
  if(!file_table){ret=OB_NOT_SUPPORTED;LOG_USER_ERROR(OB_NOT_SUPPORTED,"DESCRIBE SELECT currently requires a file table function");return ret;}
  std::string sql;
  for(int64_t i=0;i<select->get_select_item_size();++i){
    const auto&item=select->get_select_item(i);const auto&type=item.expr_->get_result_type();std::string type_name;
    switch(type.get_type()){
      case ObIntType:type_name="BIGINT";break;case ObTinyIntType:type_name="BOOLEAN";break;case ObDoubleType:type_name="DOUBLE";break;case ObDateType:type_name="DATE";break;case ObDateTimeType:type_name="DATETIME";break;default:type_name="VARCHAR";break;
    }
    bool nullable=!(type.get_result_flag()&NOT_NULL_FLAG);
    // Preserve the NULL-only logical type, which is carried as VARCHAR at runtime.
    if(item.expr_->is_column_ref_expr()){
      auto*c=static_cast<ObColumnRefRawExpr*>(item.expr_);const TableItem*t=select->get_table_item_by_id(c->get_table_id());
      if(t&&is_file_sql_expr(t->function_table_expr_)){file_sql::Spec s;std::string error;auto encoded=static_cast<const ObConstRawExpr*>(t->function_table_expr_)->get_value().get_varchar();
        if(file_sql::decode(text(encoded),s,error)){int64_t index=c->get_column_id()-OB_APP_MIN_COLUMN_ID;if(index>=0&&index<static_cast<int64_t>(s.columns.size()))type_name=file_sql::type_name(s.columns[index].type);}}
    }
    std::string name=text(item.alias_name_.empty()?item.expr_name_:item.alias_name_);
    if(i)sql+=" UNION ALL ";sql+="SELECT "+sql_text(name)+" AS column_name,"+sql_text(type_name)+" AS column_type,"+sql_text(nullable?"YES":"NO")+" AS `null`,NULL AS `key`,NULL AS `default`,NULL AS extra";
  }
  if(sql.empty())return OB_INVALID_ARGUMENT;
  ObString stored;if(OB_FAIL(ob_write_string(*params_.allocator_,ObString(sql.size(),sql.data()),stored)))return ret;
  ObParser parser(*params_.allocator_,params_.session_info_->get_sql_mode());ParseResult result;
  if(OB_FAIL(parser.parse(stored,result)))return ret;
  if(result.result_tree_==nullptr||result.result_tree_->num_child_!=1)return OB_ERR_UNEXPECTED;
  return resolve(IS_NOT_PREPARED_STMT,*result.result_tree_->children_[0],stmt);
}

int ObSelectResolver::resolve_file_copy(const ParseNode&node,ObSelectIntoItem&item)
{
  int ret=OB_SUCCESS;bool enabled=false;
  if(OB_FAIL(session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL,enabled)))return ret;
  if(!enabled){ret=OB_NOT_SUPPORTED;LOG_USER_ERROR(OB_NOT_SUPPORTED,"COPY requires enable_file_sql=ON");return ret;}
  file_sql::Spec spec;spec.options.format="csv";spec.options.header=false;
  spec.input_path=text(node.children_[0]);spec.path=spec.input_path;
  const ParseNode*options=node.children_[1];std::set<std::string>seen;std::string error;
  for(int i=0;OB_SUCC(ret)&&options&&i<options->num_child_;++i){const ParseNode*option=options->children_[i];if(!option||option->num_child_!=2)return OB_INVALID_ARGUMENT;
    std::string key=lower(text(option->children_[0]));const ParseNode*v=option->children_[1];std::string value=v?text(v):"true";
    if(v&&v->type_==T_BOOL)value=v->value_?"true":"false";
    if(!seen.insert(key).second){error="Duplicate COPY option";ret=OB_INVALID_ARGUMENT;}
    else if(key=="format"){if(lower(value)!="csv"){error="COPY supports only CSV output";ret=OB_NOT_SUPPORTED;}}
    else if(key=="header"){if(lower(value)=="true"||value=="1")spec.options.header=true;else if(lower(value)=="false"||value=="0")spec.options.header=false;else {error="COPY header must be boolean";ret=OB_INVALID_ARGUMENT;}}
    else if(key=="delimiter"||key=="delim"||key=="sep"){if(value.size()!=1||value[0]=='\n'||value[0]=='\r'||value[0]=='\0'){error="Invalid COPY delimiter";ret=OB_INVALID_ARGUMENT;}else spec.options.delimiter=value[0];}
    else if(key=="quote"||key=="escape"){if(value.size()!=1||value[0]=='\n'||value[0]=='\r'||value[0]=='\0'){error="Invalid COPY quote or escape";ret=OB_INVALID_ARGUMENT;}else if(key=="quote")spec.options.quote=value[0];else spec.options.escape=value[0];}
    else if(key=="nullstr"||key=="null_string")spec.options.nullstr=value;
    else {error="Unknown COPY option: "+key;ret=OB_INVALID_ARGUMENT;}
  }
  if(OB_SUCC(ret)&&spec.options.delimiter==spec.options.quote){error="COPY delimiter and quote must differ";ret=OB_INVALID_ARGUMENT;}
  std::string encoded;if(OB_SUCC(ret)&&!file_sql::encode(spec,encoded,error))ret=OB_INVALID_ARGUMENT;
  if(OB_SUCC(ret))ret=ob_write_string(*allocator_,ObString(encoded.size(),encoded.data()),item.external_properties_);
  if(OB_SUCC(ret))ret=resolve_into_const_node(node.children_[0],item.outfile_name_);
  if(OB_FAIL(ret)&&!error.empty()){if(ret==OB_NOT_SUPPORTED)LOG_USER_ERROR(OB_NOT_SUPPORTED,error.c_str());else LOG_USER_ERROR(OB_INVALID_ARGUMENT,error.c_str());}
  item.is_single_=true;
  return ret;
}
}} // namespace oceanbase::sql
