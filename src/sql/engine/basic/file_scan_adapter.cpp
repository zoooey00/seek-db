/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/basic/file_scan_utils.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "common/timezone/ob_time_convert.h"
#include <charconv>
#include <cmath>

namespace oceanbase { namespace sql {
using namespace common;
int ObFunctionTableOp::open_file_sql()
{
  int ret=OB_SUCCESS;bool enabled=false;ObString root;std::string error;file_sql::Spec spec;
  ObSQLSessionInfo *session=ctx_.get_my_session();
  if(session==nullptr)return OB_ERR_UNEXPECTED;
  if(OB_FAIL(session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL,enabled))){}
  else if(!enabled){ret=OB_NOT_SUPPORTED;LOG_USER_ERROR(OB_NOT_SUPPORTED,"file SQL is disabled");}
  else if(OB_FAIL(session->get_secure_file_priv(root))){}
  else if(!file_sql::decode(std::string(MY_SPEC.file_sql_config_.ptr(),MY_SPEC.file_sql_config_.length()),spec,error))ret=OB_INVALID_ARGUMENT;
  if(OB_SUCC(ret)){
    if(OB_FAIL(ObResolverUtils::check_secure_path(root,ObString(spec.path.size(),spec.path.data())))){}
    else {
      std::vector<int> projection;for(int64_t i=0;i<MY_SPEC.file_column_indices_.count();++i)projection.push_back(MY_SPEC.file_column_indices_.at(i));
      if(file_reader_==nullptr)file_reader_=new(std::nothrow)file_sql::Reader;
      if(file_reader_==nullptr)ret=OB_ALLOCATE_MEMORY_FAILED;
      else if(!file_reader_->open(spec,projection,error))ret=OB_INVALID_ARGUMENT;
    }
  }
  if(OB_FAIL(ret)&&!error.empty())LOG_USER_ERROR(OB_INVALID_ARGUMENT,error.c_str());
  return ret;
}
int ObFunctionTableOp::inner_get_next_row_file_sql()
{
  int ret=OB_SUCCESS;std::vector<file_sql::Cell>row;std::string error;bool end=false;
  clear_evaluated_flag();
  if(OB_FAIL(ctx_.check_status())){}
  else if(file_reader_==nullptr)ret=OB_NOT_INIT;
  else if(!file_reader_->next(row,end,error))ret=OB_INVALID_ARGUMENT;
  else if(end)ret=OB_ITER_END;
  for(int64_t i=0;OB_SUCC(ret)&&i<MY_SPEC.column_exprs_.count();++i){
    const int64_t index=MY_SPEC.file_column_indices_.at(i);ObExpr *expr=MY_SPEC.column_exprs_.at(i);
    if(index<0||index>=static_cast<int64_t>(row.size())){ret=OB_ERR_UNEXPECTED;break;}
    const auto&cell=row[index];ObDatum &datum=expr->locate_datum_for_write(eval_ctx_);
    if(cell.null)datum.set_null();
    else {
      const std::string &text=cell.text;ObObjType type=expr->datum_meta_.type_;
      if(type==ObIntType||type==ObTinyIntType){int64_t n=0;if(cell.type==file_sql::Type::Boolean)n=(text=="true"||text=="TRUE");else {auto r=std::from_chars(text.data(),text.data()+text.size(),n);if(r.ec!=std::errc()||r.ptr!=text.data()+text.size())ret=OB_INVALID_ARGUMENT;}datum.set_int(n);}
      else if(type==ObDoubleType){double n=0;if(cell.type==file_sql::Type::Boolean)n=(text=="true"||text=="TRUE");else {auto r=std::from_chars(text.data(),text.data()+text.size(),n);if(r.ec!=std::errc()||r.ptr!=text.data()+text.size()||!std::isfinite(n))ret=OB_INVALID_ARGUMENT;}datum.set_double(n);}
      else if(type==ObDateType){int32_t d=0;ret=ObTimeConverter::str_to_date(ObString(text.size(),text.data()),d);if(OB_SUCC(ret))datum.set_date(d);}
      else if(type==ObDateTimeType){int64_t d=0;ObTimeConvertCtx context(nullptr,false);ret=ObTimeConverter::str_to_datetime(ObString(text.size(),text.data()),context,d);if(OB_SUCC(ret))datum.set_datetime(d);}
      else {char *buffer=expr->get_str_res_mem(eval_ctx_,text.size());if(buffer==nullptr&&!text.empty())ret=OB_ALLOCATE_MEMORY_FAILED;else {if(!text.empty())MEMCPY(buffer,text.data(),text.size());datum.set_string(buffer,text.size());}}
    }
    if(OB_SUCC(ret))expr->set_evaluated_projected(eval_ctx_);
  }
  if(OB_FAIL(ret)&&ret!=OB_ITER_END){if(error.empty())error="File column conversion failed at line "+std::to_string(file_reader_?file_reader_->line_number():0);LOG_USER_ERROR(OB_INVALID_ARGUMENT,error.c_str());}
  return ret;
}
}} // namespace oceanbase::sql
