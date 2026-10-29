/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "lib/string/ob_string.h"
#include "sql/parser/parse_node.h"
#include <cstdint>
#include <string>
#include <vector>

namespace oceanbase {
namespace common { class ObMySQLProxy; }
namespace share { namespace schema { class ObColumnSchemaV2; class ObTableSchema; class AlterTableSchema; } }
namespace sql {
class ObExecContext;
class ObResolver;
class ObSQLSessionInfo;
class ObDMLStmt;
struct ObResolverParams;
namespace ai_column {
constexpr int64_t PARSE_MARKER = 0x4149434f;
constexpr const char *COMMENT_PREFIX = "seekdb:ai-column:v1:";
constexpr const char *HIDDEN_PREFIX = "__seekdb_ai_";
struct Definition {
  std::string name, model, prompt, config, expression, type, hint;
  std::vector<std::string> dependencies;
  uint64_t column_id=0, state_id=0, request_id=0, spec_id=0;
  int max_retries=2, dimension=0;
  bool embedding=false;
};
std::string string(const common::ObString &value);
std::string identifier(const std::string &value);
std::string literal(const std::string &value);
std::string state_name(uint64_t column_id);
std::string request_name(uint64_t column_id);
std::string spec_name(uint64_t column_id);
bool decode(const common::ObString &comment, Definition &definition);
bool is_result(const share::schema::ObColumnSchemaV2 &column);
bool is_owned(const share::schema::ObColumnSchemaV2 &column);
bool is_trigger_column(const share::schema::ObTableSchema &schema, const share::schema::ObColumnSchemaV2 &column);
bool strict_mode(ObSQLSessionInfo *session);
bool is_retry_node(const ParseNode &node);
int resolve_definition(share::schema::ObColumnSchemaV2 &column, const ParseNode &attribute);
int prepare_schema(share::schema::ObTableSchema &schema, const share::schema::ObTableSchema *original=nullptr);
int install(ObExecContext &ctx, const std::string &database, const std::string &table);
int start_worker(common::ObMySQLProxy *proxy);
void stop_worker();
int check_write(ObResolverParams &params, const ObDMLStmt &stmt);
int check_strict(ObExecContext &ctx, const ObDMLStmt &stmt);
} } }
