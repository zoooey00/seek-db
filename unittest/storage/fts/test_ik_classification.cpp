/* Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 */

// The runner obtains the reference header directly from the official base Git
// commit and renames its class and include guard. No reimplementation is used.
#define USING_LOG_PREFIX SHARE
#include "storage/fts/ik/ob_ik_char_util.h"
#include "reference_ik_char_util.h"
#include "reference_whitespace.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace oceanbase::common;
using namespace oceanbase::storage;
static uint64_t checked = 0;
static uint64_t streams_checked = 0;

static void check_stream(const ObCollationType coll, const std::string &input)
{
  using namespace oceanbase::share;
  ObTextWhitespaceTokenizer actual;
  ReferenceWhitespaceTokenizer expected;
  oceanbase::ObDatum document;
  // Some legacy charset handlers return an expected multibyte width even for
  // a truncated final character. Supply readable padding while keeping the
  // logical input length unchanged, so the oracle itself never reads past the
  // allocation when exercising that existing behavior.
  std::string backing = input;
  backing.append(8, '\0');
  document.set_string(backing.data(), input.size());
  const auto *cs = ObCharset::get_charset(coll);
  if (cs == nullptr) { std::fprintf(stderr, "Missing test charset %d\n", coll); std::exit(2); }
  const int a = actual.open(document, cs);
  const int b = expected.open(document, cs);
  if (a != b) std::exit(2);
  for (size_t step = 0; step <= input.size() + 2; ++step) {
    oceanbase::ObDatum left, right;
    int64_t left_freq = 791, right_freq = 791;
    const int l = actual.get_next(left, left_freq);
    const int r = expected.get_next(right, right_freq);
    if (l != r || left_freq != right_freq || (l == OB_SUCCESS &&
        (left.len_ != right.len_ || std::memcmp(left.ptr_, right.ptr_, left.len_) != 0))) {
      std::fprintf(stderr, "Token stream mismatch: case=%llu coll=%d length=%zu step=%zu rc=%d/%d\n",
                   static_cast<unsigned long long>(streams_checked), coll, input.size(), step, l, r);
      std::exit(2);
    }
    if (l != OB_SUCCESS) { ++streams_checked; return; }
  }
  std::fprintf(stderr, "Token stream failed to terminate\n");
  std::exit(2);
}

static void check(ObCollationType coll, const char *input, uint8_t length)
{
  auto actual = ObFTCharUtil::CharType::OTHER_CJK;
  auto expected = ReferenceFTCharUtil::CharType::OTHER_CJK;
  const int a = ObFTCharUtil::classify_first_char(coll, input, length, actual);
  const int b = ReferenceFTCharUtil::classify_first_char(coll, input, length, expected);
  if (a != b || static_cast<int>(actual) != static_cast<int>(expected)) {
    std::fprintf(stderr, "Mismatch: case=%llu coll=%d length=%u rc=%d/%d type=%d/%d\n",
                 static_cast<unsigned long long>(checked), coll, length, a, b,
                 static_cast<int>(actual), static_cast<int>(expected));
    std::exit(1);
  }
  ++checked;
}

int main(int argc, char **argv)
{
  OB_LOGGER.set_log_level("ERROR");
  if (ObCharset::init_charset() != OB_SUCCESS) std::exit(3);
  const ObCollationType colls[] = {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI,
                                  CS_TYPE_UTF16_BIN, CS_TYPE_UTF16LE_BIN};
  char buf[8] = {};
  for (uint32_t code = 0; code <= 0x10ffff; ++code) {
    int n = ob_charset_encode_unicode<CHARSET_UTF8MB4>(code,
             reinterpret_cast<unsigned char *>(buf), reinterpret_cast<unsigned char *>(buf + 8));
    if (n > 0) {
      check(colls[0], buf, n);
      check(colls[1], buf, n);
    }
    // All 16-bit units, including unpaired surrogate halves, in both byte orders.
    if (code <= 0xffff) {
      buf[0] = code >> 8; buf[1] = code & 255;
      check(colls[2], buf, 2);
      buf[0] = code & 255; buf[1] = code >> 8;
      check(colls[3], buf, 2);
    }
  }
  // Exhaust every two-byte pattern at every shorter available length, then use
  // a reproducible randomized corpus for malformed and truncated longer input.
  for (uint32_t value = 0; value <= 0xffff; ++value) {
    buf[0] = value >> 8; buf[1] = value & 255;
    for (auto coll : colls) for (uint8_t n = 0; n <= 2; ++n) check(coll, buf, n);
  }
  std::mt19937_64 rng(0x667473);
  for (int i = 0; i < 200000; ++i) {
    for (auto &c : buf) c = static_cast<char>(rng());
    for (auto coll : colls) check(coll, buf, rng() % 9);
  }
  for (auto coll : colls) check(coll, nullptr, 0);
  check(CS_TYPE_INVALID, buf, 1);
  check(CS_TYPE_BINARY, buf, 1);
  std::printf("PASS: %llu exact classification/result-code comparisons\n",
              static_cast<unsigned long long>(checked));
  // This official branch retains precisely these four collations. Unsupported
  // UTF16 cases above check the public classification API's error behavior;
  // they must not be reported as successful UTF16 token-stream coverage.
  const ObCollationType stream_colls[] = {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI,
      CS_TYPE_UTF8MB4_UNICODE_CI, CS_TYPE_BINARY};
  for (auto coll : stream_colls) {
    check_stream(coll, "");
    check_stream(coll, " The QUICK, brown fox--database. 数据库 ＡＢＣ１２３ 😀\t索引\n");
    for (uint32_t value = 0; value <= 0xffff; ++value) {
      std::string input;
      input += static_cast<char>(value >> 8); input += static_cast<char>(value);
      check_stream(coll, input);
      input = "abc " + input + " .xyz";
      check_stream(coll, input);
    }
    for (int i = 0; i < 20000; ++i) {
      std::string input(rng() % 129, '\0');
      for (auto &c : input) c = static_cast<char>(rng());
      check_stream(coll, input);
    }
  }
  std::printf("PASS: %llu complete whitespace token streams\n",
              static_cast<unsigned long long>(streams_checked));
  if (argc > 1 && std::strcmp(argv[1], "--benchmark") == 0) {
    // This diagnostic is separate from the official end-to-end score. Alternate
    // implementations within each group to expose drift, and keep the checksum
    // observable so the compiler cannot discard classification work.
    const char *samples[] = {"数", "据", "库", "A", "8", "，", "索", "引", "あ", "한", "😀", " ", "Ａ"};
    volatile uint64_t checksum = 0;
    for (int block = 0; block < 6; ++block) {
      for (int variant = 0; variant < 2; ++variant) {
        const bool reference = (block % 2 == 0) == (variant == 0);
        uint64_t sum = 0;
        const auto begin = std::chrono::steady_clock::now();
        for (int repeat = 0; repeat < 200000; ++repeat) {
          for (const char *sample : samples) {
            if (reference) {
              ReferenceFTCharUtil::CharType type;
              sum += ReferenceFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, sample, std::strlen(sample), type);
              sum += static_cast<int>(type);
            } else {
              ObFTCharUtil::CharType type;
              sum += ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, sample, std::strlen(sample), type);
              sum += static_cast<int>(type);
            }
          }
        }
        checksum = sum;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - begin).count();
        std::printf("classify_microseconds block=%d variant=%s elapsed=%lld checksum=%llu\n",
                    block, reference ? "reference" : "candidate", static_cast<long long>(elapsed),
                    static_cast<unsigned long long>(checksum));
      }
    }
  }
  return 0;
}
