// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.
// See http://www.apache.org/licenses/LICENSE-2.0 for license terms.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <random>
#include "storage/retrieval/ob_sparse_daat_iter.h"
#include "sql/engine/expr/ob_expr.h"
#include "share/datum/ob_datum_funcs.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::sql;

#define check(ok) do { if (!(ok)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #ok); std::abort(); } } while (0)
static int sign(int64_t v) { return (v > 0) - (v < 0); }

int main()
{
  ObArenaAllocator allocator;
  ObDatum left, right;
  ObFixedArray<const ObDatum *, ObIAllocator> ids(allocator);
  check(ids.init(2) == OB_SUCCESS);
  check(ids.push_back(&left) == OB_SUCCESS && ids.push_back(&right) == OB_SUCCESS);
  ObSRMergeItem l, r;
  l.iter_idx_ = 0; r.iter_idx_ = 1;
  ObSRMergeCmp uninitialized;
  int64_t actual = 0;
  check(uninitialized.cmp(l, r, actual) == OB_NOT_INIT);
  std::vector<std::string> corpus = {"", "a", "a ", "a  ", "A", "0", "00",
      std::string(1, '\0'), std::string(16, '\0'), std::string(16, '\xff'),
      std::string("a\0b", 3), std::string("a\0b ", 4), "数据库", "数据 库"};
  for (int c = 0; c < 256; ++c) {
    corpus.push_back(std::string(1, char(c)));
    corpus.push_back(std::string(16, char(c)));
  }
  std::mt19937 rng(20261103);
  for (int n = 0; n < 512; ++n) {
    std::string s(n % 129, '\0');
    for (char &c : s) c = char(rng() & 255);
    corpus.push_back(s);
    corpus.push_back(s + ' ');
  }
  uint64_t cases = 0;
  for (auto collation : {CS_TYPE_BINARY, CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI}) {
    ObDatumMeta meta(ObVarcharType, collation, -1);
    ObSRMergeCmp cmp;
    check(cmp.init(meta, &ids) == OB_SUCCESS);
    auto oracle = ObDatumFuncs::get_basic_func(ObVarcharType, collation)->null_first_cmp_;
    // Random binary is meaningful for CS_TYPE_BINARY; text fallbacks get valid UTF-8.
    const size_t count = collation == CS_TYPE_BINARY ? corpus.size() : 7;
    for (size_t i = 0; i <= count; ++i) {
      if (i == count) left.set_null();
      else left.set_string(ObString(corpus[i].size(), corpus[i].data()));
      for (size_t j = 0; j <= count; ++j) {
        if (j == count) right.set_null();
        else right.set_string(ObString(corpus[j].size(), corpus[j].data()));
        int expected = 0;
        check(oracle(left, right, expected, nullptr) == OB_SUCCESS);
        check(cmp.cmp(l, r, actual) == OB_SUCCESS);
        if (sign(actual) != sign(expected)) {
          std::fprintf(stderr, "MISMATCH collation=%d i=%zu j=%zu actual=%lld expected=%d\n", int(collation), i, j, (long long)actual, expected);
        }
        check(sign(actual) == sign(expected));
        ++cases;
      }
    }
  }
  ObSRMergeCmp integer_cmp;
  check(integer_cmp.init(ObDatumMeta(ObIntType, CS_TYPE_BINARY, -1), &ids) == OB_SUCCESS);
  auto integer_oracle = ObDatumFuncs::get_basic_func(ObIntType, CS_TYPE_BINARY)->null_first_cmp_;
  int64_t left_storage = 0, right_storage = 0;
  left.int_ = &left_storage;
  right.int_ = &right_storage;
  for (int64_t a : {INT64_MIN, int64_t(-1), int64_t(0), int64_t(1), INT64_MAX}) {
    for (int64_t b : {INT64_MIN, int64_t(-1), int64_t(0), int64_t(1), INT64_MAX}) {
      left.set_int(a); right.set_int(b);
      check(integer_cmp.cmp(l, r, actual) == OB_SUCCESS);
      int expected = 0;
      check(integer_oracle(left, right, expected, nullptr) == OB_SUCCESS);
      check(sign(actual) == sign(expected));
      check(sign(actual) == ((a > b) - (a < b)));
      ++cases;
    }
  }
  std::printf("PASS %llu sparse ID comparisons, null-first and fallback types\n",
      static_cast<unsigned long long>(cases));
}
