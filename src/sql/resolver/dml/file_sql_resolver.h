/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "sql/resolver/expr/ob_raw_expr.h"
#include "sql/engine/basic/file_scan_utils.h"

namespace oceanbase { namespace sql {
constexpr int64_t FILE_SQL_PARSE_MARKER = 0x4653514c;
inline bool is_file_sql_expr(const ObRawExpr *expr)
{
  if (expr == nullptr || !expr->is_const_expr()) return false;
  const common::ObObj &value = static_cast<const ObConstRawExpr *>(expr)->get_value();
  return value.is_varchar() && value.get_varchar().prefix_match(file_sql::SPEC_PREFIX);
}
}} // namespace oceanbase::sql
