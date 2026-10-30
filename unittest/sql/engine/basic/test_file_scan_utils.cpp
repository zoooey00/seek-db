/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/engine/basic/file_scan_utils.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace oceanbase::sql::file_sql;
extern "C" int write_file_sql_parquet_fixture(const char*,int);
namespace {
class FileScanTest:public ::testing::Test {
protected:
  std::string dir,error;
  void SetUp()override{char pattern[]="/var/tmp/seekdb-file-scan-test-XXXXXX";char*p=mkdtemp(pattern);ASSERT_NE(nullptr,p);dir=p;}
  void TearDown()override{if(dir.rfind("/var/tmp/seekdb-file-scan-test-",0)==0)std::filesystem::remove_all(dir);}
  std::string file(const std::string&name,const std::string&data){auto path=dir+"/"+name;std::ofstream out(path,std::ios::binary);out<<data;out.close();return path;}
  Spec schema(const std::string&name,const std::string&data,std::string format="csv"){
    Options o;o.format=format;Spec s;EXPECT_TRUE(infer(file(name,data),dir,o,s,error))<<error;return s;
  }
};
TEST_F(FileScanTest, TypesAndBoundaries){
  EXPECT_EQ(Type::Bigint,infer_type("9223372036854775807"));
  EXPECT_EQ(Type::Bigint,infer_type("-9223372036854775808"));
  EXPECT_EQ(Type::Varchar,infer_type("9223372036854775808"));
  EXPECT_EQ(Type::Varchar,infer_type("1e9999"));
  EXPECT_EQ(Type::Double,infer_type("1.25e3"));
  EXPECT_EQ(Type::Date,infer_type("2024-02-29"));
  EXPECT_EQ(Type::Varchar,infer_type("2026-02-29"));
  EXPECT_EQ(Type::Datetime,infer_type("2026-10-28T12:13:14.123456"));
  EXPECT_EQ(Type::Varchar,infer_type("2026-10-28 24:00:00"));
  EXPECT_EQ(Type::Double,widen(Type::Boolean,widen(Type::Bigint,Type::Double)));
  EXPECT_EQ(Type::Datetime,widen(Type::Date,Type::Datetime));
}
TEST_F(FileScanTest, CsvMultilineUnicodeDuplicatesAndNull){
  Spec s=schema("data.csv","\xef\xbb\xbfid,name,id,empty\r\n1,\"中文,\r\ntext\",2,\r\n3,\"say \"\"hi\"\"\",4,NULL\r\n");
  ASSERT_EQ(4U,s.columns.size());EXPECT_EQ("id__2",s.columns[2].name);EXPECT_EQ(Type::Null,s.columns[3].type);
  Reader reader;ASSERT_TRUE(reader.open(s,{0,1,2,3},error))<<error;bool end=false;std::vector<Cell> row;
  ASSERT_TRUE(reader.next(row,end,error))<<error;ASSERT_FALSE(end);EXPECT_EQ("中文,\r\ntext",row[1].text);EXPECT_TRUE(row[3].null);
  ASSERT_TRUE(reader.next(row,end,error))<<error;EXPECT_EQ("say \"hi\"",row[1].text);
  ASSERT_TRUE(reader.rescan(error));ASSERT_TRUE(reader.next(row,end,error));EXPECT_EQ("1",row[0].text);
}
TEST_F(FileScanTest, CsvOptionsAndQuotedEmpty){
  Options o;o.format="csv";o.header=false;o.delimiter=';';o.nullstr="missing";
  Spec s;ASSERT_TRUE(infer(file("data.csv","1;\"\"\n2;missing\n"),dir,o,s,error))<<error;
  EXPECT_EQ("column0",s.columns[0].name);EXPECT_EQ(Type::Varchar,s.columns[1].type);EXPECT_TRUE(s.columns[1].nullable);
  Reader r;ASSERT_TRUE(r.open(s,{0,1},error));bool end=false;std::vector<Cell> row;ASSERT_TRUE(r.next(row,end,error));EXPECT_FALSE(row[1].null);EXPECT_TRUE(row[1].text.empty());
}
TEST_F(FileScanTest, CsvMalformedRejected){
  Options o;o.format="csv";Spec s;
  EXPECT_FALSE(infer(file("bad.csv","a,b\n1\n"),dir,o,s,error));
  EXPECT_FALSE(infer(file("bad2.csv","a\n\"unfinished"),dir,o,s,error));
  EXPECT_FALSE(infer(file("bad3.csv",std::string("a\n")+char(0xff)),dir,o,s,error));
}
TEST_F(FileScanTest, JsonFullUnionMissingAndConflict){
  Spec s=schema("data.jsonl","{\"a\":1,\"b\":true}\n{\"a\":1.5,\"c\":\"2026-10-28\"}\n{\"b\":\"text\"}\n","json");
  ASSERT_EQ(3U,s.columns.size());EXPECT_EQ(Type::Double,s.columns[0].type);EXPECT_EQ(Type::Varchar,s.columns[1].type);EXPECT_EQ(Type::Date,s.columns[2].type);
  EXPECT_TRUE(s.columns[0].nullable);EXPECT_TRUE(s.columns[2].nullable);
  Reader r;ASSERT_TRUE(r.open(s,{0,1,2},error));std::vector<Cell> row;bool end=false;ASSERT_TRUE(r.next(row,end,error));EXPECT_TRUE(row[2].null);
}
TEST_F(FileScanTest, JsonNestedAndDuplicateRejected){
  Options o;o.format="json";Spec s;
  EXPECT_FALSE(infer(file("a.jsonl","{\"x\":{\"y\":1}}\n"),dir,o,s,error));
  EXPECT_FALSE(infer(file("b.jsonl","{\"x\":[1]}\n"),dir,o,s,error));
  EXPECT_FALSE(infer(file("c.jsonl","{\"x\":1,\"X\":2}\n"),dir,o,s,error));
}
TEST_F(FileScanTest, AllNullAndSchemaRoundtrip){
  Spec s=schema("null.csv","n,b\n,true\n,false\n");EXPECT_EQ(Type::Null,s.columns[0].type);EXPECT_EQ(Type::Boolean,s.columns[1].type);
  std::string encoded;ASSERT_TRUE(encode(s,encoded,error));Spec restored;ASSERT_TRUE(decode(encoded,restored,error));EXPECT_EQ(s.path,restored.path);EXPECT_EQ(s.snapshot.inode,restored.snapshot.inode);EXPECT_EQ(s.columns[0].type,restored.columns[0].type);
  EXPECT_FALSE(decode("seekdb-file-sql-v1:{}",restored,error));
}
TEST_F(FileScanTest, FileReplacementAndModificationRejected){
  Spec s=schema("data.csv","id\n1\n");file("data.csv","id\n1234\n");Reader r;EXPECT_FALSE(r.open(s,{0},error));
  s=schema("fresh.csv","id\n1\n");ASSERT_TRUE(r.open(s,{0},error));file("fresh.csv","id\n2\n");std::vector<Cell> row;bool end=false;EXPECT_FALSE(r.next(row,end,error));
}
TEST_F(FileScanTest, SymlinkEscapeAndPrefixSiblingDenied){
  std::filesystem::create_directory(dir+"/allowed");file("outside.csv","id\n1\n");symlink((dir+"/outside.csv").c_str(),(dir+"/allowed/link.csv").c_str());
  Options o;o.format="csv";Spec s;EXPECT_FALSE(infer(dir+"/allowed/link.csv",dir+"/allowed",o,s,error));
  EXPECT_FALSE(infer(dir+"/allowed/../outside.csv",dir+"/allowed",o,s,error));
  EXPECT_FALSE(infer(dir+"/outside.csv","",o,s,error));
  EXPECT_FALSE(infer(dir+"/outside.csv","NULL",o,s,error));
}
TEST_F(FileScanTest, GlobOnlyDiscoversFiles){
  file("a.csv","a\n1\n");file("b.jsonl","{\"a\":1}\n");std::filesystem::create_directory(dir+"/nested");
  Options o;o.format="glob";Spec s;ASSERT_TRUE(infer(dir+"/*",dir,o,s,error))<<error;ASSERT_EQ(2U,s.files.size());EXPECT_EQ("file",s.columns[0].name);
  Reader r;ASSERT_TRUE(r.open(s,{0},error));std::vector<Cell>row;bool end=false;ASSERT_TRUE(r.next(row,end,error));EXPECT_EQ(dir+"/a.csv",row[0].text);
}
TEST_F(FileScanTest, CsvOutputAtomicAndReadable){
  Options o;o.format="csv";o.nullstr="NULL";o.delimiter=';';std::string target=dir+"/out.csv";
  {CsvOutput out;ASSERT_TRUE(out.open(target,dir,o,error));ASSERT_TRUE(out.write({{"name",false,Type::Varchar},{"value",false,Type::Varchar}},error));ASSERT_TRUE(out.write({{"quoted;\ntext",false,Type::Varchar},{"",false,Type::Varchar}},error));EXPECT_FALSE(std::filesystem::exists(target));ASSERT_TRUE(out.commit(error))<<error;}
  Spec s;ASSERT_TRUE(infer(target,dir,o,s,error))<<error;Reader r;ASSERT_TRUE(r.open(s,{0,1},error));std::vector<Cell>row;bool end=false;ASSERT_TRUE(r.next(row,end,error));EXPECT_EQ("quoted;\ntext",row[0].text);EXPECT_FALSE(row[1].null);
  CsvOutput again;EXPECT_FALSE(again.open(target,dir,o,error));
}
TEST_F(FileScanTest, FailedOutputRemovesTemporaryFile){
  {CsvOutput out;Options o;ASSERT_TRUE(out.open(dir+"/failed.csv",dir,o,error));ASSERT_TRUE(out.write({{"data",false,Type::Varchar}},error));}
  EXPECT_TRUE(std::filesystem::is_empty(dir));
}
TEST_F(FileScanTest, ParquetFooterMultipleGroupsNullableAndProjection){
  for(int compressed=0;compressed<2;++compressed){
    std::string path=dir+"/data"+std::to_string(compressed)+".parquet";
    ASSERT_EQ(1,write_file_sql_parquet_fixture(path.c_str(),compressed));
    Options o;o.format="parquet";Spec s;ASSERT_TRUE(infer(path,dir,o,s,error))<<error;
    ASSERT_EQ(6U,s.columns.size());EXPECT_EQ(Type::Bigint,s.columns[0].type);EXPECT_EQ(Type::Boolean,s.columns[2].type);EXPECT_EQ(Type::Date,s.columns[4].type);EXPECT_EQ(Type::Datetime,s.columns[5].type);
    Reader reader;ASSERT_TRUE(reader.open(s,{0,2,5},error))<<error;
    bool end=false;int count=0;std::vector<Cell>row;
    while(!end){ASSERT_TRUE(reader.next(row,end,error))<<error;if(!end){EXPECT_EQ(std::to_string(count),row[0].text);EXPECT_TRUE(row[1].null);EXPECT_EQ(count==3,row[2].null);++count;}}
    EXPECT_EQ(7,count);ASSERT_TRUE(reader.rescan(error));ASSERT_TRUE(reader.next(row,end,error));EXPECT_EQ("0",row[0].text);
    ASSERT_TRUE(reader.open(s,{},error));count=0;end=false;while(!end){ASSERT_TRUE(reader.next(row,end,error))<<error;if(!end)++count;}EXPECT_EQ(7,count);
  }
}
TEST_F(FileScanTest, TruncatedParquetRejected){
  Options o;o.format="parquet";Spec s;EXPECT_FALSE(infer(file("truncated.parquet","PAR1garbagePAR1"),dir,o,s,error));
}
TEST_F(FileScanTest, LargeFileScannedWithoutMaterializing){
  auto path=dir+"/large.csv";{std::ofstream out(path);out<<"id,value\n";for(int i=0;i<100000;++i)out<<i<<",row\n";}
  Options o;o.format="csv";Spec s;ASSERT_TRUE(infer(path,dir,o,s,error));ASSERT_EQ(2U,s.columns.size());Reader r;ASSERT_TRUE(r.open(s,{0},error));bool end=false;std::vector<Cell>row;int64_t count=0;while(!end){ASSERT_TRUE(r.next(row,end,error))<<error;if(!end)++count;}EXPECT_EQ(100000,count);
}
TEST_F(FileScanTest, BoundedBatchScanAndBlankJsonLines){
  Spec s=schema("batch.csv","id\n1\n2\n3\n4\n5\n");Reader r;ASSERT_TRUE(r.open(s,{0},error));
  std::vector<std::vector<Cell>>batch;bool end=false;
  ASSERT_TRUE(r.next_batch(batch,3,end,error));ASSERT_EQ(3U,batch.size());EXPECT_EQ("3",batch.back()[0].text);
  ASSERT_TRUE(r.next_batch(batch,3,end,error));ASSERT_EQ(2U,batch.size());EXPECT_TRUE(end);EXPECT_EQ("4",batch[0][0].text);
  ASSERT_TRUE(r.rescan(error));ASSERT_TRUE(r.next_batch(batch,1,end,error));EXPECT_EQ("1",batch[0][0].text);
  EXPECT_FALSE(r.next_batch(batch,0,end,error));EXPECT_FALSE(r.next_batch(batch,1025,end,error));
  std::string blank(100000,'\n');blank+="{\"id\":1}\n";s=schema("blank.jsonl",blank,"json");ASSERT_TRUE(r.open(s,{0},error));ASSERT_TRUE(r.next_batch(batch,4,end,error));ASSERT_EQ(1U,batch.size());EXPECT_EQ("1",batch[0][0].text);
}
}
int main(int argc,char**argv){::testing::InitGoogleTest(&argc,argv);return RUN_ALL_TESTS();}
