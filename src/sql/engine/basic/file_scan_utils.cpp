/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_scan_utils.h"
#include "sql/engine/basic/file_scan_parquet.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <functional>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace oceanbase { namespace sql { namespace file_sql {
namespace {
std::string lower(std::string s) { for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a'-'A'; return s; }
bool fail(std::string &error, const std::string &message) { error = message; return false; }
Snapshot snapshot(const struct stat &s) {
  Snapshot t; t.device=s.st_dev; t.inode=s.st_ino; t.size=s.st_size;
  t.mtime_sec=s.st_mtim.tv_sec; t.mtime_nsec=s.st_mtim.tv_nsec;
  t.ctime_sec=s.st_ctim.tv_sec; t.ctime_nsec=s.st_ctim.tv_nsec; return t;
}
bool equal(const Snapshot &a,const Snapshot &b) {
  return a.device==b.device && a.inode==b.inode && a.size==b.size &&
    a.mtime_sec==b.mtime_sec && a.mtime_nsec==b.mtime_nsec &&
    a.ctime_sec==b.ctime_sec && a.ctime_nsec==b.ctime_nsec;
}
bool below(const std::string &path,const std::string &root) {
  return root=="/" || path==root ||
    (path.size()>root.size() && path.compare(0,root.size(),root)==0 && path[root.size()]=='/');
}
// Resolve once for policy, then traverse every canonical component without
// following links. This closes the realpath/open symlink substitution window.
int open_canonical(const std::string &path, bool directory=false) {
  int fd=::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
  size_t at=1;
  while (fd>=0 && at<path.size()) {
    size_t end=path.find('/',at); bool last=end==std::string::npos;
    std::string part=path.substr(at,last?std::string::npos:end-at);
    int next=::openat(fd,part.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK|
                     ((!last||directory)?O_DIRECTORY:0));
    ::close(fd); fd=next; if(last) break; at=end+1;
  }
  return fd;
}
bool date(const std::string &s,bool time) {
  if (s.size()<(time?19U:10U) || (!time && s.size()!=10)) return false;
  auto number=[&](size_t p,size_t n)->int {int v=0;for(size_t i=p;i<p+n;++i){if(s[i]<'0'||s[i]>'9')return -1;v=v*10+s[i]-'0';}return v;};
  if(s[4]!='-'||s[7]!='-')return false;
  int y=number(0,4),m=number(5,2),d=number(8,2);
  if(y<1||m<1||m>12||d<1)return false;
  const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  if(d>days[m-1]+(m==2&&y%4==0&&(y%100!=0||y%400==0)))return false;
  if(!time)return true;
  if((s[10]!=' '&&s[10]!='T')||s[13]!=':'||s[16]!=':')return false;
  if(number(11,2)<0||number(11,2)>23||number(14,2)<0||number(14,2)>59||number(17,2)<0||number(17,2)>59)return false;
  if(s.size()>19) {if(s[19]!='.'||s.size()<21||s.size()>26)return false;for(size_t i=20;i<s.size();++i)if(s[i]<'0'||s[i]>'9')return false;}
  return true;
}
std::string unique_name(std::string name,std::set<std::string> &used,size_t position) {
  if(name.empty())name="column"+std::to_string(position);
  std::string base=name;int suffix=1;
  while(used.count(lower(name)))name=base+"__"+std::to_string(++suffix);
  used.insert(lower(name)); return name;
}
std::string json_text(const rapidjson::Value &v) {
  rapidjson::StringBuffer b; rapidjson::Writer<rapidjson::StringBuffer> w(b);v.Accept(w);return {b.GetString(),b.GetSize()};
}
void observe(Column &column,const Cell &cell) {
  column.nullable |= cell.null;
  if(!cell.null){column.type=widen(column.type,cell.type);column.max_length=std::max(column.max_length,static_cast<int64_t>(cell.text.size()));}
}
bool write_all(int fd,const std::string &text,std::string &error) {
  size_t at=0;while(at<text.size()){ssize_t n=::write(fd,text.data()+at,text.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0)return fail(error,"CSV output write failed: "+std::string(strerror(errno)));at+=n;}return true;
}
}

bool valid_utf8(const std::string &s) {
  size_t i=0;while(i<s.size()){
    unsigned char c=s[i++];if(c<0x80){if(c==0)return false;continue;}
    int n=0;uint32_t code=0,min=0;
    if(c>=0xc2&&c<=0xdf){n=1;code=c&31;min=0x80;}
    else if(c>=0xe0&&c<=0xef){n=2;code=c&15;min=0x800;}
    else if(c>=0xf0&&c<=0xf4){n=3;code=c&7;min=0x10000;}else return false;
    if(i+n>s.size())return false;
    for(int j=0;j<n;++j){unsigned char b=s[i++];if((b&0xc0)!=0x80)return false;code=(code<<6)|(b&63);}
    if(code<min||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return false;
  }return true;
}
Type infer_type(const std::string &s) {
  if(s=="true"||s=="false"||s=="TRUE"||s=="FALSE")return Type::Boolean;
  int64_t integer=0;const char *begin=s.data(),*end=begin+s.size();
  if(begin!=end){auto r=std::from_chars(begin,end,integer);if(r.ec==std::errc()&&r.ptr==end)return Type::Bigint;}
  // An overflowing integral literal stays VARCHAR rather than silently rounding.
  bool numeric=!s.empty(), floating=false, digit=false;
  for(size_t i=0;i<s.size();++i){char c=s[i];if(c>='0'&&c<='9')digit=true;else if(c=='.'||c=='e'||c=='E')floating=true;else if(c!='+'&&c!='-')numeric=false;}
  if(numeric&&floating&&digit){double value=0;auto r=std::from_chars(begin,end,value);if(r.ec==std::errc()&&r.ptr==end&&std::isfinite(value))return Type::Double;}
  if(date(s,false))return Type::Date;
  if(date(s,true))return Type::Datetime;
  return Type::Varchar;
}
Type widen(Type a,Type b) {
  if(a==b)return a;if(a==Type::Null)return b;if(b==Type::Null)return a;
  auto numeric=[](Type t){return t==Type::Boolean||t==Type::Bigint||t==Type::Double;};
  if(numeric(a)&&numeric(b))return a==Type::Double||b==Type::Double?Type::Double:Type::Bigint;
  if((a==Type::Date&&b==Type::Datetime)||(b==Type::Date&&a==Type::Datetime))return Type::Datetime;
  return Type::Varchar;
}
const char *type_name(Type t) {
  switch(t){case Type::Null:return "NULL";case Type::Bigint:return "BIGINT";case Type::Double:return "DOUBLE";case Type::Boolean:return "BOOLEAN";case Type::Date:return "DATE";case Type::Datetime:return "DATETIME";default:return "VARCHAR";}
}
bool canonical_path(const std::string &input,const std::string &secure_root,
                    std::string &path,std::string &root,std::string &error) {
  if(input.empty()||input.find('\0')!=std::string::npos||input.find("://")!=std::string::npos)
    return fail(error,"File SQL requires a nonempty local path without NUL bytes");
  if(secure_root.empty()||lower(secure_root)=="null"||secure_root.find('\0')!=std::string::npos)
    return fail(error,"File SQL denied by secure_file_priv");
  char p[4096],r[4096];
  if(!::realpath(secure_root.c_str(),r))return fail(error,"secure_file_priv is not an existing directory");
  struct stat st;if(::stat(r,&st)||!S_ISDIR(st.st_mode))return fail(error,"secure_file_priv is not a directory");
  if(!::realpath(input.c_str(),p))return fail(error,"Cannot resolve file path: "+input);
  path=p;root=r;if(!below(path,root))return fail(error,"File SQL path is outside secure_file_priv: "+input);
  return true;
}

class Reader::Impl {
public:
  Spec spec;std::vector<int> projection;FILE *file=nullptr;int fd=-1;void *parquet=nullptr;
  int64_t line=1;size_t glob_index=0;bool first=true;
  std::vector<Cell> pending;
  ~Impl(){close();}
  void close(){if(parquet){file_parquet_close(parquet);parquet=nullptr;}if(file){fclose(file);file=nullptr;fd=-1;}else if(fd>=0){::close(fd);fd=-1;}}
  bool unchanged(std::string &error) const {
    if(spec.options.format=="glob")return true;
    struct stat s;char resolved[4096];
    if(fd<0||fstat(fd,&s)||!equal(snapshot(s),spec.snapshot)||
       !realpath(spec.input_path.c_str(),resolved)||spec.path!=resolved||
       stat(spec.path.c_str(),&s)||!equal(snapshot(s),spec.snapshot))
      return fail(error,"File changed since schema inference: "+spec.input_path);
    return true;
  }
  bool csv(std::vector<Cell> &row,bool &end,std::string &error) {
    row.clear();end=false;std::string value;bool quoted=false,in_quote=false,after_quote=false,any=false;size_t bytes=0;
    auto push=[&](){Cell c;c.text=value;c.null=!quoted&&(value.empty()||value==spec.options.nullstr||value=="\\N"||value=="NULL");c.type=c.null?Type::Null:infer_type(value);row.push_back(std::move(c));value.clear();quoted=false;after_quote=false;};
    while(true){int ch=fgetc(file);if(ch==EOF){if(ferror(file))return fail(error,"CSV read failed at line "+std::to_string(line));if(in_quote)return fail(error,"Unterminated quoted CSV field at line "+std::to_string(line));if(!any&&row.empty()&&value.empty()){end=true;return true;}push();break;}
      any=true;if(++bytes>MAX_RECORD_BYTES)return fail(error,"CSV record exceeds 8 MiB at line "+std::to_string(line));char c=static_cast<char>(ch);
      if(first){first=false;if(static_cast<unsigned char>(c)==0xef){int b=fgetc(file),d=fgetc(file);if(b!=0xbb||d!=0xbf)return fail(error,"Invalid UTF-8 BOM");continue;}}
      if(in_quote){
        if(c==spec.options.quote){int n=fgetc(file);if(n==spec.options.quote&&spec.options.escape==spec.options.quote)value+=c;else {in_quote=false;after_quote=true;if(n!=EOF)ungetc(n,file);}}
        else if(c==spec.options.escape&&spec.options.escape!=spec.options.quote){int n=fgetc(file);if(n==EOF)return fail(error,"Truncated CSV escape at line "+std::to_string(line));value+=static_cast<char>(n);}
        else if(c=='\r'){int n=fgetc(file);if(n!='\n'&&n!=EOF)ungetc(n,file);value+='\n';++line;}
        else {value+=c;if(c=='\n')++line;}
      }else if(c==spec.options.delimiter){push();if(row.size()>=MAX_COLUMNS)return fail(error,"CSV has too many columns");}
      else if(c=='\n'||c=='\r'){if(c=='\r'){int n=fgetc(file);if(n!='\n'&&n!=EOF)ungetc(n,file);}++line;push();break;}
      else if(c==spec.options.quote&&value.empty()&&!after_quote){quoted=true;in_quote=true;}
      else if(after_quote||c==spec.options.quote)return fail(error,"Malformed CSV quoting at line "+std::to_string(line));
      else value+=c;
    }
    for(const Cell &c:row)if(!valid_utf8(c.text))return fail(error,"Invalid UTF-8 in CSV at line "+std::to_string(line));
    return true;
  }
  bool json(std::vector<std::pair<std::string,Cell>> &fields,bool &end,std::string &error){
    fields.clear();end=false;std::string text;int ch=EOF;
    for(;;){
      text.clear();
      while((ch=fgetc(file))!=EOF){if(ch=='\n'){++line;break;}text+=static_cast<char>(ch);if(text.size()>MAX_RECORD_BYTES)return fail(error,"JSONL record exceeds 8 MiB at line "+std::to_string(line));}
      if(ch==EOF&&ferror(file))return fail(error,"JSONL read failed");
      if(ch==EOF&&text.empty()){end=true;return true;}
      if(first){first=false;if(text.compare(0,3,"\xef\xbb\xbf")==0)text.erase(0,3);}
      if(text.find_first_not_of(" \t\r")!=std::string::npos)break;
    }
    rapidjson::Document doc;doc.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(),text.size());
    if(doc.HasParseError()||!doc.IsObject())return fail(error,"Expected a flat JSON object at line "+std::to_string(line-(ch=='\n')));
    if(doc.MemberCount()>MAX_COLUMNS)return fail(error,"JSONL has too many fields");std::set<std::string> names;
    for(auto i=doc.MemberBegin();i!=doc.MemberEnd();++i){
      std::string name(i->name.GetString(),i->name.GetStringLength());
      if(name.empty()||!valid_utf8(name)||!names.insert(lower(name)).second)return fail(error,"Empty, invalid or duplicate JSONL field name at line "+std::to_string(line));
      const auto &v=i->value;Cell c;c.null=v.IsNull();
      if(v.IsObject()||v.IsArray())return fail(error,"Nested JSON objects and arrays are not supported at line "+std::to_string(line));
      if(v.IsString()){c.text.assign(v.GetString(),v.GetStringLength());Type t=infer_type(c.text);c.type=(t==Type::Date||t==Type::Datetime)?t:Type::Varchar;}
      else if(!c.null){c.text=json_text(v);c.type=v.IsBool()?Type::Boolean:v.IsInt64()?Type::Bigint:v.IsDouble()?Type::Double:Type::Varchar;}
      if(!valid_utf8(c.text))return fail(error,"Invalid JSONL UTF-8 or NUL text");fields.emplace_back(name,std::move(c));
    }return true;
  }
};
Reader::Reader():impl_(new Impl){}
Reader::~Reader()=default;
void Reader::close(){impl_->close();}
int64_t Reader::line_number()const{return impl_->line;}
bool Reader::check_unchanged(std::string &e)const{return impl_->unchanged(e);}
bool Reader::open(const Spec &s,const std::vector<int> &projection,std::string &error){
  close();impl_->pending.clear();impl_->spec=s;impl_->projection=projection;impl_->line=1;impl_->first=true;impl_->glob_index=0;
  for(int i:projection)if(i<0||static_cast<size_t>(i)>=s.columns.size())return fail(error,"Invalid file scan projection");
  if(s.options.format=="glob")return true;
  std::string p,r;if(!canonical_path(s.input_path,s.root,p,r,error)||p!=s.path||r!=s.root)return fail(error,"File path changed since inference");
  impl_->fd=open_canonical(s.path);struct stat st;
  if(impl_->fd<0||fstat(impl_->fd,&st)||!S_ISREG(st.st_mode)){close();return fail(error,"File SQL input is not a readable regular file");}
  if(!impl_->unchanged(error)){close();return false;}
  if(s.options.format=="parquet"){
    char e[2048]={0};impl_->parquet=file_parquet_open(dup(impl_->fd),e,sizeof(e));
    if(!impl_->parquet||!file_parquet_project(impl_->parquet,projection.data(),static_cast<int>(projection.size()),e,sizeof(e))){close();return fail(error,e);}
  }else{
    impl_->file=fdopen(impl_->fd,"rb");if(!impl_->file){close();return fail(error,"Cannot open file stream");}
    if(s.options.format=="csv"&&s.options.header){std::vector<Cell> header;bool end=false;if(!impl_->csv(header,end,error)){close();return false;}}
  }return true;
}
bool Reader::rescan(std::string &error){Spec s=impl_->spec;auto p=impl_->projection;return open(s,p,error);}
bool Reader::next(std::vector<Cell> &row,bool &end,std::string &error){
  end=false;row.clear();if(!impl_->unchanged(error))return false;
  if(!impl_->pending.empty()){row=std::move(impl_->pending);impl_->pending.clear();return true;}
  const Spec &s=impl_->spec;
  if(s.options.format=="glob"){
    if(impl_->glob_index==s.files.size()){end=true;return true;}
    std::string p,r;if(!canonical_path(s.files[impl_->glob_index],s.root,p,r,error))return false;
    Cell c;c.null=false;c.text=s.files[impl_->glob_index++];c.type=Type::Varchar;row.push_back(std::move(c));
  }else if(s.options.format=="parquet"){
    char e[2048]={0};int done=0;if(!file_parquet_next(impl_->parquet,&done,e,sizeof(e)))return fail(error,e);end=done;if(end)return impl_->unchanged(error);
    row.resize(s.columns.size());
    for(size_t j=0;j<impl_->projection.size();++j){const char *text=nullptr;size_t n=0;int null=0;if(!file_parquet_cell(impl_->parquet,static_cast<int>(j),&text,&n,&null,e,sizeof(e)))return fail(error,e);Cell &c=row[impl_->projection[j]];c.null=null;c.type=s.columns[impl_->projection[j]].type;if(!null)c.text.assign(text,n);}
  }else if(s.options.format=="csv"){
    if(!impl_->csv(row,end,error))return false;
    if(!end&&row.size()!=s.columns.size())return fail(error,"CSV field count mismatch at line "+std::to_string(impl_->line));
  }else{
    std::vector<std::pair<std::string,Cell>> fields;if(!impl_->json(fields,end,error))return false;if(end)return impl_->unchanged(error);row.resize(s.columns.size());
    for(auto &field:fields){bool found=false;for(size_t i=0;i<s.columns.size();++i)if(field.first==s.columns[i].name){row[i]=std::move(field.second);found=true;break;}if(!found)return fail(error,"JSONL schema changed during scan");}
  }
  size_t bytes=0;for(const auto&cell:row){if(cell.text.size()>MAX_RECORD_BYTES-bytes)return fail(error,"File row exceeds 8 MiB");bytes+=cell.text.size();}
  return impl_->unchanged(error);
}
bool Reader::next_batch(std::vector<std::vector<Cell>>&rows,size_t max_rows,bool&end,std::string&error){
  rows.clear();end=false;if(max_rows<1||max_rows>1024)return fail(error,"File batch size must be between 1 and 1024");
  size_t bytes=0;
  while(rows.size()<max_rows&&!end){std::vector<Cell>row;if(!next(row,end,error))return false;if(end)break;
    size_t row_bytes=0;for(const auto&cell:row)row_bytes+=cell.text.size();
    if(!rows.empty()&&row_bytes>16*1024*1024-bytes){impl_->pending=std::move(row);break;}
    bytes+=row_bytes;rows.push_back(std::move(row));
  }return true;
}

bool infer(const std::string &input,const std::string &secure_root,const Options &o,Spec &s,std::string &error){
  s=Spec();s.options=o;s.input_path=input;
  if(o.format!="csv"&&o.format!="json"&&o.format!="parquet"&&o.format!="glob")return fail(error,"Unsupported file format");
  if(o.format=="glob"){
    if(input.find('\0')!=std::string::npos||input.find("**")!=std::string::npos)return fail(error,"Invalid glob pattern");
    size_t wildcard=input.find_first_of("*?[");size_t slash=wildcard==std::string::npos?std::string::npos:input.rfind('/',wildcard);
    std::string prefix=wildcard==std::string::npos?input:slash==std::string::npos?".":slash==0?"/":input.substr(0,slash);
    std::string p,r;if(!canonical_path(prefix,secure_root,p,r,error))return false;s.root=r;s.path=p;
    size_t bytes=0,visited=0;
    auto add=[&](const std::string&candidate){
      std::string actual,root;if(!canonical_path(candidate,secure_root,actual,root,error))return false;
      struct stat st;if(stat(actual.c_str(),&st)||!S_ISREG(st.st_mode))return true;
      bytes+=candidate.size();if(s.files.size()>=10000||bytes>MAX_SCHEMA_BYTES)return fail(error,"glob discovery resource limit exceeded");
      s.files.push_back(candidate);return true;
    };
    if(wildcard==std::string::npos){if(!add(input))return false;}
    else {
      const size_t start=slash==std::string::npos?0:slash+1;
      std::vector<std::string>components;size_t at=start;
      while(at<input.size()){size_t next=input.find('/',at);components.push_back(input.substr(at,next==std::string::npos?std::string::npos:next-at));if(next==std::string::npos)break;at=next+1;}
      if(components.size()>64)return fail(error,"glob path has too many components");
      std::function<bool(const std::string&,const std::string&,size_t)> walk;
      walk=[&](const std::string&directory,const std::string&display,size_t index){
        int fd=open_canonical(directory,true);if(fd<0)return fail(error,"Cannot open glob directory");DIR*dir=fdopendir(fd);
        if(!dir){::close(fd);return fail(error,"Cannot scan glob directory");}
        bool ok=true;errno=0;
        while(ok){struct dirent*entry=readdir(dir);if(!entry){if(errno)ok=fail(error,"glob directory read failed");break;}
          if(++visited>100000){ok=fail(error,"glob directory entry limit exceeded");break;}
          const std::string name=entry->d_name;if(name=="."||name==".."||fnmatch(components[index].c_str(),name.c_str(),FNM_PERIOD)!=0)continue;
          if(!valid_utf8(name)){ok=fail(error,"Invalid UTF-8 glob filename");break;}
          std::string candidate=directory+"/"+name,actual,root;
          if(!canonical_path(candidate,secure_root,actual,root,error)){ok=false;break;}
          struct stat st;if(stat(actual.c_str(),&st)){ok=fail(error,"glob file changed during discovery");break;}
          std::string shown=display+name;
          if(index+1==components.size()){if(S_ISREG(st.st_mode))ok=add(shown);}
          else if(S_ISDIR(st.st_mode))ok=walk(actual,shown+"/",index+1);
          errno=0;
        }
        closedir(dir);return ok;
      };
      std::string display=input.substr(0,start);
      if(!walk(p,display,0))return false;
    }
    std::sort(s.files.begin(),s.files.end());s.columns.push_back({"file",Type::Varchar,false,4096});return true;
  }
  if(!canonical_path(input,secure_root,s.path,s.root,error))return false;
  Reader reader;reader.impl_->spec=s;
  // A descriptor is captured from the open descriptor, never from unverified input.
  reader.impl_->fd=open_canonical(s.path);struct stat st;
  if(reader.impl_->fd<0||fstat(reader.impl_->fd,&st)||!S_ISREG(st.st_mode))return fail(error,"File SQL input must be a readable regular file");
  s.snapshot=snapshot(st);reader.impl_->spec=s;
  if(o.format=="parquet"){
    char e[2048]={0};void *pq=file_parquet_open(dup(reader.impl_->fd),e,sizeof(e));if(!pq)return fail(error,e);
    int n=file_parquet_column_count(pq);std::set<std::string> names;
    for(int i=0;i<n;++i){const char *name=nullptr;int type=0,nullable=0;if(!file_parquet_column(pq,i,&name,&type,&nullable)){file_parquet_close(pq);return fail(error,"Unsupported Parquet schema");}Type t=static_cast<Type>(type);s.columns.push_back({unique_name(name,names,i),t,nullable!=0,t==Type::Datetime?26:t==Type::Date?10:65535});}
    file_parquet_close(pq);return reader.impl_->unchanged(error);
  }
  reader.impl_->file=fdopen(reader.impl_->fd,"rb");if(!reader.impl_->file)return fail(error,"Cannot open input stream");
  bool end=false;int64_t rows=0;size_t schema_bytes=0;std::set<std::string> names;
  while(!end){
    if(o.format=="csv"){
      std::vector<Cell> row;if(!reader.impl_->csv(row,end,error))return false;if(end)break;
      if(s.columns.empty()){
        for(size_t i=0;i<row.size();++i){Column c;c.name=unique_name(o.header?row[i].text:"column"+std::to_string(i),names,i);schema_bytes+=c.name.size();s.columns.push_back(std::move(c));}
        if(o.header)continue;
      }
      if(row.size()!=s.columns.size())return fail(error,"CSV field count mismatch at line "+std::to_string(reader.impl_->line));
      for(size_t i=0;i<row.size();++i)observe(s.columns[i],row[i]);
    }else{
      std::vector<std::pair<std::string,Cell>> fields;if(!reader.impl_->json(fields,end,error))return false;if(end)break;std::set<size_t> present;
      for(auto &field:fields){size_t index=0;while(index<s.columns.size()&&s.columns[index].name!=field.first)++index;
        if(index==s.columns.size()){if(s.columns.size()>=MAX_COLUMNS)return fail(error,"JSONL union has too many columns");if(!names.insert(lower(field.first)).second)return fail(error,"JSONL field names differ only by case");Column c;c.name=field.first;c.nullable=rows>0;schema_bytes+=c.name.size();s.columns.push_back(std::move(c));}
        observe(s.columns[index],field.second);present.insert(index);
      }
      for(size_t i=0;i<s.columns.size();++i)if(!present.count(i))s.columns[i].nullable=true;
    }
    if(schema_bytes>MAX_SCHEMA_BYTES)return fail(error,"File schema resource limit exceeded");
    ++rows;if((rows&255)==0&&!reader.impl_->unchanged(error))return false;
  }
  if(s.columns.empty())return fail(error,"Cannot infer columns from an empty file");
  for(Column &c:s.columns)if(c.type==Type::Null)c.nullable=true;
  return reader.impl_->unchanged(error);
}

bool encode(const Spec &s,std::string &encoded,std::string &error){
  rapidjson::StringBuffer b;rapidjson::Writer<rapidjson::StringBuffer>w(b);
  w.StartObject();auto text=[&](const char*k,const std::string&v){w.Key(k);w.String(v.data(),static_cast<rapidjson::SizeType>(v.size()));};
  text("input",s.input_path);text("path",s.path);text("root",s.root);text("format",s.options.format);text("nullstr",s.options.nullstr);
  w.Key("header");w.Bool(s.options.header);w.Key("delimiter");w.Int(s.options.delimiter);w.Key("quote");w.Int(s.options.quote);w.Key("escape");w.Int(s.options.escape);
  w.Key("stat");w.StartArray();w.Uint64(s.snapshot.device);w.Uint64(s.snapshot.inode);w.Int64(s.snapshot.size);w.Int64(s.snapshot.mtime_sec);w.Int64(s.snapshot.mtime_nsec);w.Int64(s.snapshot.ctime_sec);w.Int64(s.snapshot.ctime_nsec);w.EndArray();
  w.Key("columns");w.StartArray();for(const Column&c:s.columns){w.StartArray();w.String(c.name.data(),static_cast<rapidjson::SizeType>(c.name.size()));w.Int(static_cast<int>(c.type));w.Bool(c.nullable);w.Int64(c.max_length);w.EndArray();}w.EndArray();
  w.Key("files");w.StartArray();for(const auto&f:s.files)w.String(f.data(),static_cast<rapidjson::SizeType>(f.size()));w.EndArray();w.EndObject();
  if(b.GetSize()>MAX_SCHEMA_BYTES*2)return fail(error,"Encoded file schema is too large");encoded=SPEC_PREFIX;encoded.append(b.GetString(),b.GetSize());return true;
}
bool decode(const std::string &encoded,Spec &s,std::string &error){
  size_t prefix=strlen(SPEC_PREFIX);if(encoded.compare(0,prefix,SPEC_PREFIX)!=0||encoded.size()>MAX_SCHEMA_BYTES*2+prefix)return fail(error,"Invalid file scan descriptor");
  rapidjson::Document d;d.Parse(encoded.data()+prefix,encoded.size()-prefix);if(d.HasParseError()||!d.IsObject())return fail(error,"Malformed file scan descriptor");
  for(const char*k:{"input","path","root","format","nullstr"})if(!d.HasMember(k)||!d[k].IsString())return fail(error,"Missing file scan descriptor field");
  for(const char*k:{"delimiter","quote","escape"})if(!d.HasMember(k)||!d[k].IsInt())return fail(error,"Invalid file scan option");
  if(!d.HasMember("header")||!d["header"].IsBool()||!d.HasMember("stat")||!d["stat"].IsArray()||d["stat"].Size()!=7||!d.HasMember("columns")||!d["columns"].IsArray()||d["columns"].Size()>MAX_COLUMNS||!d.HasMember("files")||!d["files"].IsArray())return fail(error,"Invalid file scan schema");
  s=Spec();s.input_path=d["input"].GetString();s.path=d["path"].GetString();s.root=d["root"].GetString();s.options.format=d["format"].GetString();s.options.nullstr=d["nullstr"].GetString();s.options.header=d["header"].GetBool();s.options.delimiter=d["delimiter"].GetInt();s.options.quote=d["quote"].GetInt();s.options.escape=d["escape"].GetInt();
  auto&a=d["stat"];for(rapidjson::SizeType i=0;i<7;++i)if(!a[i].IsInt64()&&!a[i].IsUint64())return fail(error,"Invalid file snapshot");s.snapshot.device=a[0].GetUint64();s.snapshot.inode=a[1].GetUint64();s.snapshot.size=a[2].GetInt64();s.snapshot.mtime_sec=a[3].GetInt64();s.snapshot.mtime_nsec=a[4].GetInt64();s.snapshot.ctime_sec=a[5].GetInt64();s.snapshot.ctime_nsec=a[6].GetInt64();
  for(auto&i:d["columns"].GetArray()){if(!i.IsArray()||i.Size()!=4||!i[0].IsString()||!i[1].IsInt()||i[1].GetInt()<0||i[1].GetInt()>6||!i[2].IsBool()||!i[3].IsInt64())return fail(error,"Invalid file column");s.columns.push_back({i[0].GetString(),static_cast<Type>(i[1].GetInt()),i[2].GetBool(),i[3].GetInt64()});}
  for(auto&i:d["files"].GetArray()){if(!i.IsString()||s.files.size()>=10000)return fail(error,"Invalid glob discovery");s.files.emplace_back(i.GetString());}return true;
}

CsvOutput::CsvOutput()=default;
CsvOutput::~CsvOutput(){close();}
void CsvOutput::close(){if(fd_>=0){::close(fd_);fd_=-1;}if(parent_fd_>=0){if(!temporary_.empty())unlinkat(parent_fd_,temporary_.c_str(),0);::close(parent_fd_);parent_fd_=-1;}temporary_.clear();}
bool CsvOutput::open(const std::string &path,const std::string &secure_root,const Options&o,std::string &error){
  close();if(path.empty()||path.find('\0')!=std::string::npos)return fail(error,"Invalid COPY target");size_t slash=path.rfind('/');std::string dir=slash==std::string::npos?".":slash==0?"/":path.substr(0,slash);target_=slash==std::string::npos?path:path.substr(slash+1);if(target_.empty()||target_=="."||target_=="..")return fail(error,"Invalid COPY target filename");
  std::string resolved,root;if(!canonical_path(dir,secure_root,resolved,root,error))return false;parent_fd_=open_canonical(resolved,true);if(parent_fd_<0)return fail(error,"Cannot open COPY directory");struct stat st;if(fstatat(parent_fd_,target_.c_str(),&st,AT_SYMLINK_NOFOLLOW)==0||errno!=ENOENT)return fail(error,"COPY target already exists or is inaccessible");
  options_=o;for(int i=0;i<32&&fd_<0;++i){temporary_=".seekdb-copy-"+std::to_string(getpid())+"-"+std::to_string(reinterpret_cast<uintptr_t>(this))+"-"+std::to_string(i);fd_=openat(parent_fd_,temporary_.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd_<0&&errno!=EEXIST)break;}
  if(fd_<0){temporary_.clear();return fail(error,"Cannot create COPY temporary file");}return true;
}
bool CsvOutput::write(const std::vector<Cell>&row,std::string &error){
  if(fd_<0)return fail(error,"COPY output is not open");std::string record;
  for(size_t i=0;i<row.size();++i){if(i)record+=options_.delimiter;const Cell&c=row[i];if(c.null){record+=options_.nullstr;continue;}bool quote=c.text.empty()||c.text==options_.nullstr||c.text=="NULL"||c.text=="\\N"||c.text.find_first_of(std::string()+options_.delimiter+options_.quote+"\r\n")!=std::string::npos;
    if(quote)record+=options_.quote;for(char ch:c.text){if(quote&&(ch==options_.quote||(ch==options_.escape&&options_.escape!=options_.quote)))record+=options_.escape;record+=ch;}if(quote)record+=options_.quote;
    if(record.size()>MAX_RECORD_BYTES)return fail(error,"COPY record exceeds 8 MiB");
  }record+='\n';return write_all(fd_,record,error);
}
bool CsvOutput::commit(std::string &error){
  if(fd_<0||parent_fd_<0)return fail(error,"COPY output is not open");if(fsync(fd_)!=0)return fail(error,"COPY flush failed");
  if(linkat(parent_fd_,temporary_.c_str(),parent_fd_,target_.c_str(),0)!=0)return fail(error,"COPY cannot publish target (it may already exist)");
  unlinkat(parent_fd_,temporary_.c_str(),0);temporary_.clear();::close(fd_);fd_=-1;return true;
}
}}} // namespace oceanbase::sql::file_sql
