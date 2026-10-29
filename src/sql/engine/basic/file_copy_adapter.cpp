/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_select_into_op.h"
#include "sql/engine/basic/file_scan_utils.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "share/ob_lob_access_utils.h"

namespace oceanbase { namespace sql {
using namespace common;
int ObSelectIntoOp::open_file_copy()
{
  int ret=OB_SUCCESS;ObSQLSessionInfo*session=ctx_.get_my_session();bool enabled=false;ObString root;
  std::string error;file_sql::Spec config;const ObString encoded=MY_SPEC.external_properties_.str_;
  if(!session)return OB_ERR_UNEXPECTED;
  if(OB_FAIL(session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL,enabled)))return ret;
  if(!enabled){ret=OB_NOT_SUPPORTED;LOG_USER_ERROR(OB_NOT_SUPPORTED,"COPY requires enable_file_sql=ON");return ret;}
  if(OB_FAIL(session->get_secure_file_priv(root)))return ret;
  if(!file_sql::decode(std::string(encoded.ptr(),encoded.length()),config,error))ret=OB_INVALID_ARGUMENT;
  if(OB_SUCC(ret)){
    delete copy_output_;copy_output_=new(std::nothrow)file_sql::CsvOutput;
    if(!copy_output_)ret=OB_ALLOCATE_MEMORY_FAILED;
    else if(!copy_output_->open(config.path,std::string(root.ptr(),root.length()),config.options,error))ret=OB_INVALID_ARGUMENT;
    else if(config.options.header){std::vector<file_sql::Cell>header;for(const auto&c:config.columns)header.push_back({c.name,false,file_sql::Type::Varchar});if(!copy_output_->write(header,error))ret=OB_INVALID_ARGUMENT;}
  }
  copy_done_=false;opened_=true;
  if(OB_FAIL(ret)&&!error.empty())LOG_USER_ERROR(OB_INVALID_ARGUMENT,error.c_str());
  return ret;
}
int ObSelectIntoOp::write_file_copy_row()
{
  int ret=OB_SUCCESS;std::vector<file_sql::Cell>row;std::string error;
  for(int64_t i=0;OB_SUCC(ret)&&i<MY_SPEC.select_exprs_.count();++i){
    ObExpr*expr=MY_SPEC.select_exprs_.at(i);ObDatum*datum=nullptr;ObObj obj;file_sql::Cell cell;
    if(OB_FAIL(expr->eval(eval_ctx_,datum))){}
    else if(datum->is_null())cell.null=true;
    else if(OB_FAIL(datum->to_obj(obj,expr->obj_meta_))){}
    else {
      cell.null=false;cell.type=file_sql::Type::Varchar;
      if(obj.is_lob_storage()){
        // LOBs must be read through the engine's locator API; never print a locator as file data.
        ObTextStringIter iter(expr->datum_meta_.type_,expr->datum_meta_.cs_type_,datum->get_string(),expr->obj_meta_.has_lob_header());
        ObEvalCtx::TempAllocGuard guard(eval_ctx_);ObTextStringIterState state;ObString block;
        if(OB_FAIL(ObTextStringHelper::build_text_iter(iter,ctx_,&guard.get_allocator()))){}
        else {
          while(OB_SUCC(ret)&&(state=iter.get_next_block(block))==TEXTSTRING_ITER_NEXT){
            if(cell.text.size()+block.length()>file_sql::MAX_RECORD_BYTES)ret=OB_SIZE_OVERFLOW;
            else cell.text.append(block.ptr(),block.length());
          }
          if(OB_SUCC(ret)&&state!=TEXTSTRING_ITER_END)ret=OB_INVALID_ARGUMENT;
        }
      }else if(obj.is_string_type()){ObString value=obj.get_string();cell.text.assign(value.ptr(),value.length());}
      else {char buffer[4096];int64_t pos=0;if(OB_FAIL(obj.print_plain_str_literal(buffer,sizeof(buffer),pos))){}else cell.text.assign(buffer,pos);}
    }
    if(OB_SUCC(ret))row.push_back(std::move(cell));
  }
  if(OB_SUCC(ret)&&!copy_output_->write(row,error)){ret=OB_INVALID_ARGUMENT;LOG_USER_ERROR(OB_INVALID_ARGUMENT,error.c_str());}
  return ret;
}
int ObSelectIntoOp::run_file_copy(bool batch)
{
  int ret=OB_SUCCESS;std::string error;int64_t count=0;bool end=copy_done_;
  if(!copy_output_)return OB_NOT_INIT;
  if(copy_done_)return OB_ITER_END;
  while(OB_SUCC(ret)&&!end){
    clear_evaluated_flag();if(OB_FAIL(ctx_.check_status()))break;
    if(batch){const ObBatchRows*rows=nullptr;if(OB_FAIL(child_->get_next_batch(1024,rows))){}
      else {ObEvalCtx::BatchInfoScopeGuard guard(eval_ctx_);guard.set_batch_size(rows->size_);
        for(int64_t i=0;OB_SUCC(ret)&&i<rows->size_;++i)if(!rows->skip_->contain(i)){guard.set_batch_idx(i);ret=write_file_copy_row();if(OB_SUCC(ret))++count;}
        end=rows->end_;
      }
    }else if(OB_FAIL(child_->get_next_row())){if(ret==OB_ITER_END){ret=OB_SUCCESS;end=true;}}
    else {ret=write_file_copy_row();if(OB_SUCC(ret))++count;}
  }
  if(OB_SUCC(ret)&&OB_FAIL(ctx_.check_status())){}
  if(OB_SUCC(ret)&&!copy_output_->commit(error)){ret=OB_INVALID_ARGUMENT;LOG_USER_ERROR(OB_INVALID_ARGUMENT,error.c_str());}
  if(OB_FAIL(ret))copy_output_->close();
  else {copy_done_=true;ctx_.get_physical_plan_ctx()->set_affected_rows(count);ret=OB_ITER_END;}
  return ret;
}
}} // namespace oceanbase::sql
