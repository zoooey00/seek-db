/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_scan_parquet.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <unistd.h>

namespace {
struct Decoder {
  void *library=nullptr;
  decltype(&file_parquet_open)open=nullptr;
  decltype(&file_parquet_column_count)count=nullptr;
  decltype(&file_parquet_column)column=nullptr;
  decltype(&file_parquet_project)project=nullptr;
  decltype(&file_parquet_next)next=nullptr;
  decltype(&file_parquet_cell)cell=nullptr;
  decltype(&file_parquet_close)close=nullptr;
  std::string error;
};
Decoder decoder;
std::once_flag initialized;
void initialize()
{
  std::string paths[4];
#ifdef FILE_SQL_PARQUET_LIBRARY
  paths[0]=FILE_SQL_PARQUET_LIBRARY;
#endif
  char executable[4096];ssize_t n=readlink("/proc/self/exe",executable,sizeof(executable)-1);
  if(n>0){executable[n]=0;std::string path=executable;size_t slash=path.rfind('/');path.resize(slash);
    paths[1]=path+"/../lib/libseekdb_file_parquet.so";
    paths[2]=path+"/../sql/libseekdb_file_parquet.so";
    paths[3]=path+"/libseekdb_file_parquet.so";
  }
  int flags=RTLD_NOW|RTLD_LOCAL;
#ifdef RTLD_DEEPBIND
  flags|=RTLD_DEEPBIND;
#endif
  for(const auto&path:paths)if(!path.empty()){
    decoder.library=dlopen(path.c_str(),flags);if(decoder.library)break;
    const char *message=dlerror();if(message)decoder.error=message;
  }
  if(!decoder.library)return;
#define FILE_SQL_SYMBOL(member,name) decoder.member=reinterpret_cast<decltype(decoder.member)>(dlsym(decoder.library,name));
  FILE_SQL_SYMBOL(open,"file_parquet_open")
  FILE_SQL_SYMBOL(count,"file_parquet_column_count")
  FILE_SQL_SYMBOL(column,"file_parquet_column")
  FILE_SQL_SYMBOL(project,"file_parquet_project")
  FILE_SQL_SYMBOL(next,"file_parquet_next")
  FILE_SQL_SYMBOL(cell,"file_parquet_cell")
  FILE_SQL_SYMBOL(close,"file_parquet_close")
#undef FILE_SQL_SYMBOL
  if(!decoder.open||!decoder.count||!decoder.column||!decoder.project||!decoder.next||!decoder.cell||!decoder.close){
    decoder.error="Parquet decoder has an incompatible C interface";decoder.open=nullptr;
  }
}
}
extern "C" void *file_parquet_open(int fd,char*error,size_t size){
  std::call_once(initialized,initialize);
  if(!decoder.open){if(fd>=0)::close(fd);if(error&&size)snprintf(error,size,"Cannot load local Parquet decoder: %s",decoder.error.c_str());return nullptr;}
  return decoder.open(fd,error,size);
}
extern "C" int file_parquet_column_count(void*p){return decoder.count?decoder.count(p):0;}
extern "C" int file_parquet_column(void*p,int i,const char**name,int*type,int*nullable){return decoder.column?decoder.column(p,i,name,type,nullable):0;}
extern "C" int file_parquet_project(void*p,const int*indices,int count,char*error,size_t size){return decoder.project?decoder.project(p,indices,count,error,size):0;}
extern "C" int file_parquet_next(void*p,int*end,char*error,size_t size){return decoder.next?decoder.next(p,end,error,size):0;}
extern "C" int file_parquet_cell(void*p,int i,const char**text,size_t*length,int*null,char*error,size_t size){return decoder.cell?decoder.cell(p,i,text,length,null,error,size):0;}
extern "C" void file_parquet_close(void*p){if(decoder.close)decoder.close(p);}
