/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// OceanBase's packaged Arrow/Parquet libraries use libstdc++ ABI 0. Only this
// format-decoder translation unit uses that ABI; the SQL engine uses ABI 1.
#ifdef _GLIBCXX_USE_CXX11_ABI
#undef _GLIBCXX_USE_CXX11_ABI
#endif
#define _GLIBCXX_USE_CXX11_ABI 0
#include "sql/engine/basic/file_scan_parquet.h"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
class LimitedPool final : public arrow::MemoryPool {
public:
  arrow::ProxyMemoryPool pool{arrow::system_memory_pool()};
  static constexpr int64_t LIMIT=128*1024*1024;
  arrow::Status Allocate(int64_t n,uint8_t **p)override {
    if(n<0||n>LIMIT-pool.bytes_allocated())return arrow::Status::OutOfMemory("Parquet scan exceeds 128 MiB");
    return pool.Allocate(n,p);
  }
  arrow::Status Reallocate(int64_t old,int64_t n,uint8_t **p)override {
    if(n<0||n-old>LIMIT-pool.bytes_allocated())return arrow::Status::OutOfMemory("Parquet scan exceeds 128 MiB");
    return pool.Reallocate(old,n,p);
  }
  void Free(uint8_t*p,int64_t n)override{pool.Free(p,n);}
  int64_t bytes_allocated()const override{return pool.bytes_allocated();}
  int64_t max_memory()const override{return pool.max_memory();}
  std::string backend_name()const override{return "seekdb-file-sql-limited";}
};
struct ParquetState {
  LimitedPool pool;
  std::shared_ptr<arrow::io::ReadableFile> file;
  std::unique_ptr<parquet::arrow::FileReader> reader;
  std::shared_ptr<arrow::Schema> schema;
  std::unique_ptr<arrow::RecordBatchReader> batches;
  std::shared_ptr<arrow::RecordBatch> batch;
  int64_t at=0,total=0,seen=0;
  std::string cell;
  bool metadata_only=false;
};
int error(char*out,size_t n,const std::string &message){if(out&&n)snprintf(out,n,"%s",message.c_str());return 0;}
int sql_type(const std::shared_ptr<arrow::DataType>&t){
  switch(t->id()){
    case arrow::Type::NA:return 0;
    case arrow::Type::INT8:case arrow::Type::INT16:case arrow::Type::INT32:case arrow::Type::INT64:
    case arrow::Type::UINT8:case arrow::Type::UINT16:case arrow::Type::UINT32:return 1;
    case arrow::Type::FLOAT:case arrow::Type::DOUBLE:return 2;
    case arrow::Type::BOOL:return 3;
    case arrow::Type::STRING:case arrow::Type::LARGE_STRING:case arrow::Type::BINARY:
    case arrow::Type::LARGE_BINARY:case arrow::Type::UINT64:return 4;
    case arrow::Type::DATE32:case arrow::Type::DATE64:return 5;
    case arrow::Type::TIMESTAMP:{auto timestamp=std::static_pointer_cast<arrow::TimestampType>(t);return timestamp->timezone().empty()?6:-1;}
    default:return -1;
  }
}
}
extern "C" void *file_parquet_open(int owned_fd,char*out,size_t size){
  std::unique_ptr<ParquetState>s(new ParquetState);
  try{
    struct stat st;unsigned char tail[8]={0},head[4]={0};
    if(owned_fd<0||fstat(owned_fd,&st)||st.st_size<12||pread(owned_fd,head,4,0)!=4||
       pread(owned_fd,tail,8,st.st_size-8)!=8||memcmp(head,"PAR1",4)||memcmp(tail+4,"PAR1",4)){
      if(owned_fd>=0)::close(owned_fd);error(out,size,"Invalid Parquet magic or truncated footer");return nullptr;
    }
    uint32_t footer=static_cast<uint32_t>(tail[0])|(static_cast<uint32_t>(tail[1])<<8)|
      (static_cast<uint32_t>(tail[2])<<16)|(static_cast<uint32_t>(tail[3])<<24);
    if(footer>16*1024*1024||footer>st.st_size-12){::close(owned_fd);error(out,size,"Parquet footer resource limit or size exceeded");return nullptr;}
    auto opened=arrow::io::ReadableFile::Open(owned_fd,&s->pool);
    if(!opened.ok()){::close(owned_fd);error(out,size,opened.status().ToString());return nullptr;}
    s->file=*opened;
    auto status=parquet::arrow::OpenFile(s->file,&s->pool,&s->reader);
    if(!status.ok()){error(out,size,status.ToString());return nullptr;}
    s->reader->set_batch_size(1024);s->reader->set_use_threads(false);
    status=s->reader->GetSchema(&s->schema);
    if(!status.ok()){error(out,size,status.ToString());return nullptr;}
    if(s->schema->num_fields()<1||s->schema->num_fields()>1024){error(out,size,"Parquet column count is outside supported limits");return nullptr;}
    for(const auto&field:s->schema->fields())if(sql_type(field->type())<0){error(out,size,"Nested or unsupported Parquet column: "+field->name());return nullptr;}
    auto meta=s->reader->parquet_reader()->metadata();s->total=meta->num_rows();
    if(meta->num_row_groups()>100000){error(out,size,"Parquet row group resource limit exceeded");return nullptr;}
    return s.release();
  }catch(const std::exception&e){error(out,size,e.what());return nullptr;}
}
extern "C" int file_parquet_column_count(void*p){return p?static_cast<ParquetState*>(p)->schema->num_fields():0;}
extern "C" int file_parquet_column(void*p,int i,const char**name,int*type,int*nullable){
  auto*s=static_cast<ParquetState*>(p);if(!s||i<0||i>=s->schema->num_fields())return 0;
  const auto&f=s->schema->field(i);*name=f->name().c_str();*type=sql_type(f->type());*nullable=f->nullable();return *type>=0;
}
extern "C" int file_parquet_project(void*p,const int*indices,int count,char*out,size_t n){
  auto*s=static_cast<ParquetState*>(p);if(!s||count<0)return error(out,n,"Invalid Parquet scan");
  try{
    s->metadata_only=count==0;if(s->metadata_only)return 1;
    std::vector<int>columns(indices,indices+count),groups;
    auto meta=s->reader->parquet_reader()->metadata();
    for(int i:columns)if(i<0||i>=s->schema->num_fields())return error(out,n,"Invalid projected Parquet column");
    for(int i=0;i<meta->num_row_groups();++i){groups.push_back(i);int64_t bytes=0;
      for(int c:columns){int64_t z=meta->RowGroup(i)->ColumnChunk(c)->total_uncompressed_size();if(z<0||z>128*1024*1024-bytes)return error(out,n,"Projected Parquet row group exceeds 128 MiB");bytes+=z;}}
    auto status=s->reader->GetRecordBatchReader(groups,columns,&s->batches);
    return status.ok()?1:error(out,n,status.ToString());
  }catch(const std::exception&e){return error(out,n,e.what());}
}
extern "C" int file_parquet_next(void*p,int*end,char*out,size_t n){
  auto*s=static_cast<ParquetState*>(p);if(!s)return error(out,n,"Parquet reader is closed");
  try{
    if(s->metadata_only){*end=s->seen++>=s->total;return 1;}
    while(!s->batch||s->at>=s->batch->num_rows()){
      auto status=s->batches->ReadNext(&s->batch);s->at=0;
      if(!status.ok())return error(out,n,status.ToString());
      if(!s->batch){*end=1;return 1;}
    }
    ++s->at;++s->seen;*end=0;return 1;
  }catch(const std::exception&e){return error(out,n,e.what());}
}
extern "C" int file_parquet_cell(void*p,int i,const char**text,size_t*length,int*is_null,char*out,size_t n){
  auto*s=static_cast<ParquetState*>(p);
  if(!s||!s->batch||i<0||i>=s->batch->num_columns()||s->at<1)return error(out,n,"Invalid Parquet row access");
  try{
    auto scalar=s->batch->column(i)->GetScalar(s->at-1);if(!scalar.ok())return error(out,n,scalar.status().ToString());
    *is_null=!(*scalar)->is_valid;s->cell=*is_null?"":(*scalar)->ToString();
    if(s->cell.size()>8*1024*1024)return error(out,n,"Parquet field exceeds 8 MiB");
    *text=s->cell.data();*length=s->cell.size();return 1;
  }catch(const std::exception&e){return error(out,n,e.what());}
}
extern "C" void file_parquet_close(void*p){delete static_cast<ParquetState*>(p);}
