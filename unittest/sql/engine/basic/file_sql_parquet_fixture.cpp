/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#ifdef _GLIBCXX_USE_CXX11_ABI
#undef _GLIBCXX_USE_CXX11_ABI
#endif
#define _GLIBCXX_USE_CXX11_ABI 0
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

extern "C" int write_file_sql_parquet_fixture(const char *path,int compressed)
{
  arrow::Int64Builder ids;arrow::StringBuilder names;arrow::BooleanBuilder active;
  arrow::DoubleBuilder scores;arrow::Date32Builder days;
  arrow::TimestampBuilder timestamps(arrow::timestamp(arrow::TimeUnit::MICRO),arrow::system_memory_pool());
  for(int i=0;i<7;++i){
    if(!ids.Append(i).ok()||!names.Append(i%2?"中文,quoted":"same").ok())return 0;
    if(i==3){if(!active.AppendNull().ok()||!scores.AppendNull().ok())return 0;}
    else if(!active.Append(i%2==0).ok()||!scores.Append(i+0.5).ok())return 0;
    if(!days.Append(20000+i).ok()||!timestamps.Append((20000LL+i)*86400*1000000+123456).ok())return 0;
  }
  std::vector<std::shared_ptr<arrow::Array>>arrays(6);
  if(!ids.Finish(&arrays[0]).ok()||!names.Finish(&arrays[1]).ok()||!active.Finish(&arrays[2]).ok()||
     !scores.Finish(&arrays[3]).ok()||!days.Finish(&arrays[4]).ok()||!timestamps.Finish(&arrays[5]).ok())return 0;
  auto schema=arrow::schema({arrow::field("id",arrow::int64(),false),arrow::field("name",arrow::utf8()),
    arrow::field("active",arrow::boolean()),arrow::field("score",arrow::float64()),
    arrow::field("day",arrow::date32()),arrow::field("created",arrow::timestamp(arrow::TimeUnit::MICRO))});
  auto table=arrow::Table::Make(schema,arrays);
  auto opened=arrow::io::FileOutputStream::Open(path);if(!opened.ok())return 0;
  parquet::WriterProperties::Builder options;
  options.compression(compressed?parquet::Compression::SNAPPY:parquet::Compression::UNCOMPRESSED);
  if(!compressed)options.disable_dictionary();
  auto status=parquet::arrow::WriteTable(*table,arrow::system_memory_pool(),*opened,2,options.build());
  if(!status.ok())return 0;return (*opened)->Close().ok();
}
