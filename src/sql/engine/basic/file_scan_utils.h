/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oceanbase { namespace sql { namespace file_sql {

enum class Type { Null, Bigint, Double, Boolean, Varchar, Date, Datetime };
struct Cell {
  std::string text;
  bool null = true;
  Type type = Type::Null;
};
struct Column {
  std::string name;
  Type type = Type::Null;
  bool nullable = false;
  int64_t max_length = 0;
};
struct Options {
  std::string format;
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string nullstr;
};
struct Snapshot {
  uint64_t device = 0, inode = 0;
  int64_t size = 0, mtime_sec = 0, mtime_nsec = 0, ctime_sec = 0, ctime_nsec = 0;
};
struct Spec {
  std::string input_path, path, root;
  Options options;
  Snapshot snapshot;
  std::vector<Column> columns;
  // glob discovers names only; it never concatenates file contents.
  std::vector<std::string> files;
};
constexpr size_t MAX_COLUMNS = 1024;
constexpr size_t MAX_RECORD_BYTES = 8 * 1024 * 1024;
constexpr size_t MAX_SCHEMA_BYTES = 4 * 1024 * 1024;
constexpr const char *SPEC_PREFIX = "seekdb-file-sql-v1:";

Type infer_type(const std::string &value);
Type widen(Type lhs, Type rhs);
const char *type_name(Type type);
bool valid_utf8(const std::string &value);
bool canonical_path(const std::string &input, const std::string &secure_root,
                    std::string &path, std::string &root, std::string &error);
bool infer(const std::string &path, const std::string &secure_root, const Options &options,
           Spec &spec, std::string &error);
bool encode(const Spec &spec, std::string &encoded, std::string &error);
bool decode(const std::string &encoded, Spec &spec, std::string &error);

class Reader {
public:
  Reader();
  ~Reader();
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;
  bool open(const Spec &spec, const std::vector<int> &projection, std::string &error);
  bool next(std::vector<Cell> &row, bool &end, std::string &error);
  bool next_batch(std::vector<std::vector<Cell>> &rows, size_t max_rows,
                  bool &end, std::string &error);
  bool rescan(std::string &error);
  bool check_unchanged(std::string &error) const;
  void close();
  int64_t line_number() const;
private:
  friend bool infer(const std::string &, const std::string &, const Options &, Spec &, std::string &);
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Transactional CSV output. A target is published with a no-replace link only
// after the complete native query succeeds; destruction removes incomplete data.
class CsvOutput {
public:
  CsvOutput();
  ~CsvOutput();
  bool open(const std::string &path, const std::string &secure_root,
            const Options &options, std::string &error);
  bool write(const std::vector<Cell> &row, std::string &error);
  bool commit(std::string &error);
  void close();
private:
  int fd_ = -1, parent_fd_ = -1;
  std::string target_, temporary_;
  Options options_;
};

}}} // namespace oceanbase::sql::file_sql
