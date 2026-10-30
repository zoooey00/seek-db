/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <ctime>

// The runner extracts this unmodified oracle from the official base commit.
#define ObFTCharUtil LegacyFTCharUtil
#include "reference_ik_char_util.h"
#undef ObFTCharUtil
#undef _OCEANBASE_STORAGE_FTS_IK_OB_IK_CHAR_UTIL_H_
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ik/ob_ik_arbitrator.h"
#include "storage/fts/ik/ob_ik_processor.h"
#include "storage/fts/ob_beng_ft_parser.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "data_plane/fts/ob_fts_parser_property.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::share;

static void require(bool ok, const char *message)
{
  if (!ok) { throw std::runtime_error(message); }
}

// A freeing allocator makes lifetime failures visible which an arena's no-op
// free can conceal. It also detects leaks and double frees in these tests.
class TestAllocator final : public ObIAllocator
{
public:
  std::map<void *, int64_t> blocks;
  int64_t calls = 0;
  int64_t fail_after = INT64_MAX;
  void *alloc(int64_t size, const oceanbase::lib::ObMemAttr &) override { return alloc(size); }
  void *alloc(int64_t size) override
  {
    if (calls >= fail_after) { return nullptr; }
    void *p = std::malloc(size);
    if (p != nullptr) { blocks[p] = size; ++calls; }
    return p;
  }
  void free(void *p) override
  {
    if (p != nullptr) {
      auto it = blocks.find(p);
      require(it != blocks.end(), "invalid or duplicate free");
      std::memset(p, 0xa5, it->second);
      blocks.erase(it);
      std::free(p);
    }
  }
  ~TestAllocator() override
  {
    for (const auto &block : blocks) { std::free(block.first); }
  }
};

static int encode(uint32_t code, int encoding, char *out)
{
  if (encoding == 0) {
    if (code < 0x80) { out[0] = code; return 1; }
    if (code < 0x800) {
      out[0] = 0xc0 | (code >> 6); out[1] = 0x80 | (code & 63); return 2;
    }
    if (code < 0x10000) {
      out[0] = 0xe0 | (code >> 12); out[1] = 0x80 | ((code >> 6) & 63);
      out[2] = 0x80 | (code & 63); return 3;
    }
    out[0] = 0xf0 | (code >> 18); out[1] = 0x80 | ((code >> 12) & 63);
    out[2] = 0x80 | ((code >> 6) & 63); out[3] = 0x80 | (code & 63); return 4;
  }
  uint16_t words[2];
  int count = 1;
  if (code < 0x10000) { words[0] = code; }
  else {
    code -= 0x10000; count = 2;
    words[0] = 0xd800 | (code >> 10); words[1] = 0xdc00 | (code & 1023);
  }
  for (int i = 0; i < count; ++i) {
    out[2 * i + (encoding == 1 ? 0 : 1)] = words[i] >> 8;
    out[2 * i + (encoding == 1 ? 1 : 0)] = words[i] & 255;
  }
  return count * 2;
}

static void test_character_classification()
{
  const ObCollationType collations[] = {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF16_BIN, CS_TYPE_UTF16LE_BIN};
  uint64_t compared = 0;
  for (int encoding = 0; encoding < 3; ++encoding) {
    for (uint32_t code = 0; code <= 0x10ffff; ++code) {
      if (code >= 0xd800 && code <= 0xdfff) { continue; }
      char bytes[4];
      int len = encode(code, encoding, bytes);
      LegacyFTCharUtil::CharType old_type;
      ObFTCharUtil::CharType new_type;
      int before = LegacyFTCharUtil::classify_first_char(collations[encoding], bytes, len, old_type);
      int after = ObFTCharUtil::classify_first_char(collations[encoding], bytes, len, new_type);
      require(before == after, "character classification error changed");
      require(static_cast<int>(old_type) == static_cast<int>(new_type), "character category changed");
      ++compared;
    }
  }
  const std::vector<std::string> malformed = {
    "", std::string("\x80", 1), std::string("\xc0\xaf", 2),
    std::string("\xe4\xb8", 2), std::string("\xed\xa0\x80", 3),
    std::string("\xf4\x90\x80\x80", 4), std::string("\xff", 1)
  };
  for (const auto &bytes : malformed) {
    for (auto collation : collations) {
      LegacyFTCharUtil::CharType old_type;
      ObFTCharUtil::CharType new_type;
      int before = LegacyFTCharUtil::classify_first_char(collation, bytes.data(), bytes.size(), old_type);
      int after = ObFTCharUtil::classify_first_char(collation, bytes.data(), bytes.size(), new_type);
      require(before == after, "malformed-character error changed");
      require(static_cast<int>(old_type) == static_cast<int>(new_type), "malformed-character category changed");
    }
  }
  std::printf("PASS character classification: %llu scalar/encoding pairs + malformed inputs\n",
              static_cast<unsigned long long>(compared));
}

static void test_result_cell_lifetime()
{
  TestAllocator allocator;
  const char text[] = "alpha beta";
  {
    TokenizeContext ctx(CS_TYPE_UTF8MB4_BIN, allocator, text, sizeof(text) - 1, false);
    require(ctx.init() == OB_SUCCESS, "context initialization failed");
    ObIKToken a{text, 0, 5, 5, ObIKTokenType::IK_ENGLISH_TOKEN};
    ObIKToken b{text, 6, 4, 4, ObIKTokenType::IK_ENGLISH_TOKEN};
    require(ctx.result_list().push_back(a) == OB_SUCCESS, "push result failed");
    require(ctx.result_list().push_back(b) == OB_SUCCESS, "push result failed");
    const char *word = nullptr;
    int64_t len = 0, offset = 0, chars = 0;
    require(ctx.get_next_token(word, len, offset, chars) == OB_SUCCESS, "first result failed");
    require(word == text && len == 5 && offset == 0 && chars == 5, "released list cell was read");
    require(ctx.get_next_token(word, len, offset, chars) == OB_SUCCESS, "last result failed");
    require(word == text && len == 4 && offset == 6 && chars == 4, "last list cell was read after free");
    require(ctx.get_next_token(word, len, offset, chars) == OB_ITER_END, "missing result end");
  }
  require(allocator.blocks.empty(), "result-cell storage leaked");
  {
    const char quantity[] = "110个";
    TokenizeContext ctx(CS_TYPE_UTF8MB4_BIN, allocator, quantity, sizeof(quantity) - 1, true);
    require(ctx.init() == OB_SUCCESS, "quantity context initialization failed");
    ObIKToken number{quantity, 0, 3, 3, ObIKTokenType::IK_ARABIC_TOKEN};
    ObIKToken unit{quantity, 3, 3, 1, ObIKTokenType::IK_COUNT_TOKEN};
    require(ctx.result_list().push_back(number) == OB_SUCCESS, "push number failed");
    require(ctx.result_list().push_back(unit) == OB_SUCCESS, "push unit failed");
    const char *word = nullptr;
    int64_t len = 0, offset = 0, chars = 0;
    require(ctx.get_next_token(word, len, offset, chars) == OB_SUCCESS, "quantity compound failed");
    require(word == quantity && len == 6 && chars == 4, "wrong quantity compound");
    require(ctx.result_list().empty(), "compound must consume the final unit");
  }
  require(allocator.blocks.empty(), "quantity-cell storage leaked");
  std::puts("PASS result-cell lifetime with a freeing allocator");
}

static void test_chain_lookup()
{
  for (int count : {0, 1, 15, 16, 17, 64, 513}) {
    for (bool smart : {false, true}) {
      ObArenaAllocator allocator;
      std::string text;
      for (int i = 0; i < count; ++i) { text += "abc "; }
      text += "!";
      TokenizeContext ctx(CS_TYPE_UTF8MB4_BIN, allocator, text.data(), text.size(), smart);
      require(ctx.init() == OB_SUCCESS, "chain context init failed");
      // Deliberately add in reverse order; token sorting must preserve output.
      for (int i = count - 1; i >= 0; --i) {
        require(ctx.add_token(text.data(), i * 4, 3, 3, ObIKTokenType::IK_ENGLISH_TOKEN)
                == OB_SUCCESS, "add chain token failed");
      }
      ObIKArbitrator arb;
      require(arb.process(ctx) == OB_SUCCESS, "arbitration failed");
      require(arb.output_result(ctx) == OB_SUCCESS, "chain lookup failed");
      for (int i = 0; i < count; ++i) {
        const char *word = nullptr;
        int64_t len = 0, offset = 0, chars = 0;
        require(ctx.get_next_token(word, len, offset, chars) == OB_SUCCESS, "missing chain result");
        require(offset == i * 4 && len == 3 && chars == 3, "wrong chain result order");
      }
      require(ctx.result_list().empty(), "extra chain results");
    }
  }
  std::puts("PASS chain lookup: empty, inline capacity boundaries, spill, reverse token insertion, both modes");
}

static void test_beng_token_lifetime()
{
  TestAllocator allocator;
  std::string text = "Alpha ALPHA beta !!! The CaSe ";
  text += std::string(4096, 'X');
  std::vector<std::pair<const char *, int64_t>> retained;
  std::vector<std::string> expected = {"alpha", "alpha", "beta", "the", "case", std::string(4096, 'x')};
  {
    ObBEngFTParser parser(allocator);
    ObFTParserParam param;
    param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
    param.allocator_ = &allocator;
    param.fulltext_ = text.data(); param.ft_length_ = text.size();
    require(parser.init(&param) == OB_SUCCESS, "beng init failed");
    const char *word = nullptr;
    int64_t len = 0, chars = 0, freq = 0;
    for (const auto &value : expected) {
      require(parser.get_next_token(word, len, chars, freq) == OB_SUCCESS, "missing beng token");
      require(std::string(word, len) == value && freq == 1 && chars == len, "wrong beng token");
      retained.emplace_back(word, len);
    }
    require(parser.get_next_token(word, len, chars, freq) == OB_ITER_END, "unexpected beng token");
    parser.reset();
  }
  for (size_t i = 0; i < expected.size(); ++i) {
    require(std::string(retained[i].first, retained[i].second) == expected[i],
            "retained beng token expired on advance/reset/destruction");
  }
  std::puts("PASS beng token lifetime, normalization, repeated words and 4096-byte tokens");
}

static void test_range_dict_cleanup()
{
  for (int count : {0, 1, 8, 9, 64}) {
    TestAllocator allocator;
    {
      ObFTDAT dat;
      ObDictCacheValue value(&dat);
      ObFTCacheRangeContainer container(allocator);
      for (int i = 0; i < count; ++i) {
        ObFTCacheRangeHandle *handle = nullptr;
        require(container.fetch_info_for_dict(handle) == OB_SUCCESS, "cache handle allocation failed");
        handle->value_ = &value;
      }
      const size_t pinned_blocks = allocator.blocks.size();
      ObFTDictDesc desc("test", ObFTDictType::DICT_IK_MAIN, CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN);
      // Inject failures at every allocation, including the inline-array spill.
      for (int allowed = 0; allowed <= count + 5; ++allowed) {
        allocator.fail_after = allocator.calls + allowed;
        {
          ObFTRangeDict dict(allocator, &container, desc);
          const int ret = dict.init();
          require(ret == OB_SUCCESS || ret == OB_ALLOCATE_MEMORY_FAILED, "unexpected range init error");
        }
        require(allocator.blocks.size() == pinned_blocks, "range wrapper storage leaked");
      }
      allocator.fail_after = INT64_MAX;
    }
    require(allocator.blocks.empty(), "cache handles leaked");
  }
  std::puts("PASS range dictionaries: destruction, capacity boundaries and allocation failures");
}

static void test_word_frequency_updates()
{
  for (const auto collation : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI}) {
    ObArenaAllocator allocator;
    ObFTWordMap words;
    require(words.create(2, "FTFrequencyTest") == OB_SUCCESS, "word map init failed");
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(collation);
    ObFTParserProperty property;
    ObAddWordFlag flag;
    flag.set_groupby_word();
    ObAddWord add(property, meta, flag, allocator, words);
    // Deliberately force many bucket collisions; every first occurrence stays
    // at the legacy value 1, and later occurrences add their supplied frequency.
    std::vector<std::string> tokens;
    tokens.reserve(257);
    for (int i = 0; i < 257; ++i) {
      tokens.push_back("word" + std::to_string(i));
      const auto &word = tokens.back();
      require(add.process_word(word.data(), word.size(), word.size(), 3) == OB_SUCCESS,
              "first frequency insertion failed");
      require(add.process_word(word.data(), word.size(), word.size(), 5) == OB_SUCCESS,
              "frequency update failed");
    }
    for (const auto &word : tokens) {
      int64_t frequency = 0;
      require(words.get_refactored(ObFTWord(word.size(), word.data(), meta), frequency) == OB_SUCCESS
              && frequency == 6, "word frequency semantics changed");
    }
    require(add.process_word("Case", 4, 4, 2) == OB_SUCCESS, "case insertion failed");
    require(add.process_word("case", 4, 4, 7) == OB_SUCCESS, "case update failed");
    int64_t frequency = 0;
    require(words.get_refactored(ObFTWord(4, "Case", meta), frequency) == OB_SUCCESS
            && frequency == (collation == CS_TYPE_UTF8MB4_GENERAL_CI ? 8 : 1),
            "word map collation or retained key changed");
    require(add.process_word("word", 4, 4, 0) == OB_INVALID_ARGUMENT,
            "invalid frequency must be rejected");
  }
  std::puts("PASS one-pass word frequency updates: collisions, grouped frequencies and collations");
}

// Independent oracle: retain the original conversion and collation-aware set.
class LegacyStopwordOracle
{
public:
  LegacyStopwordOracle()
  {
    meta.set_varchar();
    meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    require(words.create(37, "StopwordOracle", "StopwordOracle") == OB_SUCCESS, "oracle init failed");
    for (const auto &text : ob_stop_word_list) {
      require(words.set_refactored(ObFTWord(std::strlen(text), text, meta)) == OB_SUCCESS,
              "oracle insertion failed");
    }
    inited_ = true;
  }
  __attribute__((noinline)) int check(const ObFTWord &word, bool &found)
  {
    if (!inited_) { return OB_NOT_INIT; }
    if (word.empty()) { return OB_INVALID_ARGUMENT; }
    ObArenaAllocator allocator;
    ObString converted;
    int ret = ObCharset::charset_convert(allocator, word.get_word().get_string(),
        word.get_collation_type(), meta.get_collation_type(), converted);
    if (ret == OB_SUCCESS) {
      ret = words.exist_refactored(ObFTWord(converted.length(), converted.ptr(), meta));
      if (ret == OB_HASH_EXIST || ret == OB_HASH_NOT_EXIST) {
        found = ret == OB_HASH_EXIST;
        ret = OB_SUCCESS;
      } else if (ret == OB_SUCCESS) { ret = OB_ERR_UNEXPECTED; }
    }
    return ret;
  }
private:
  oceanbase::common::hash::ObHashSet<ObFTWord> words;
  ObObjMeta meta;
  bool inited_ = false;
};

static void test_stopword_equivalence()
{
  ObStopWordChecker checker;
  LegacyStopwordOracle oracle;
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  bool found = false;
  require(checker.check_stopword(ObFTWord(3, "the", meta), found) == OB_NOT_INIT,
          "uninitialized checker accepted word");
  require(checker.init() == OB_SUCCESS, "stopword init failed");
  require(checker.init() == OB_INIT_TWICE, "double init accepted");
  require(checker.check_stopword(ObFTWord(), found) == OB_INVALID_ARGUMENT,
          "empty stopword accepted");
  std::vector<std::string> samples = {" ", "   ", "notstop", "longword", "longword ",
      "caf\xc3\xa9", "th\xc3\xa9", "\xe4\xb8\xad\xe6\x96\x87", "\xc3", "\xff",
      std::string("the\0", 4), std::string("th\0e", 4), "the\t", "the\n"};
  for (const auto &text : ob_stop_word_list) {
    std::string base(text);
    for (unsigned mask = 0; mask < (1U << base.size()); ++mask) {
      std::string word(base);
      for (size_t i = 0; i < word.size(); ++i) {
        if (mask & (1U << i)) { word[i] -= 'a' - 'A'; }
      }
      samples.push_back(word);
      samples.push_back(word + " ");
      samples.push_back(word + "        ");
    }
    for (int c = 0; c < 128; ++c) {
      samples.push_back(base + char(c));
      samples.push_back(std::string(1, char(c)) + base);
    }
  }
  // Deterministic arbitrary ASCII, including embedded NUL and lengths 7/8.
  uint32_t state = 1234567;
  for (int i = 0; i < 10000; ++i) {
    std::string word(1 + i % 12, ' ');
    for (auto &c : word) { state = state * 1664525U + 1013904223U; c = (state >> 24) & 127; }
    samples.push_back(word);
  }
  int64_t cases = 0;
  for (const auto cs : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI,
                        CS_TYPE_UTF8MB4_UNICODE_CI, CS_TYPE_LATIN1_SWEDISH_CI,
                        CS_TYPE_GBK_CHINESE_CI, CS_TYPE_BINARY}) {
    meta.set_collation_type(cs);
    for (const auto &text : samples) {
      ObFTWord word(text.size(), text.data(), meta);
      bool expected = false, actual = false;
      const int old_ret = oracle.check(word, expected);
      const int new_ret = checker.check_stopword(word, actual);
      if (old_ret != new_ret || (old_ret == OB_SUCCESS && expected != actual)) {
        std::fprintf(stderr, "stopword mismatch collation=%d len=%zu old=%d/%d new=%d/%d\n",
                     int(cs), text.size(), old_ret, expected, new_ret, actual);
        require(false, "stopword semantics changed");
      }
      ++cases;
    }
  }
  checker.destroy();
  checker.destroy();
  require(checker.init() == OB_SUCCESS, "stopword reinit failed");
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  require(checker.check_stopword(ObFTWord(4, "THE ", meta), found) == OB_SUCCESS && found,
          "reinitialized checker lost stopword");
  std::printf("PASS stopword equivalence: %ld comparisons across six collations, lifecycle and fallback\n", cases);
}

static double thread_cpu_seconds()
{
  timespec now{};
  require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0, "thread clock failed");
  return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static void benchmark_stopwords()
{
  ObStopWordChecker checker;
  LegacyStopwordOracle oracle;
  require(checker.init() == OB_SUCCESS, "benchmark checker init failed");
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  const std::vector<std::vector<std::string>> workloads = {
      {"the", "THE ", "a", "where", "with", "www"},
      {"ocean", "abc", "query", "index", "hello", "abcdefg"},
      {"database", "longword123", "\xe4\xb8\xad\xe6\x96\x87", "caf\xc3\xa9", "th\xc3\xa9"}};
  const char *names[] = {"ascii_hit", "ascii_miss", "fallback"};
  constexpr int rounds = 5000000;
  for (size_t w = 0; w < workloads.size(); ++w) {
    int64_t expected_hits = -1;
    for (const bool fast : {false, true, true, false}) {
      int64_t hits = 0;
      const double cpu_start = thread_cpu_seconds();
      for (int i = 0; i < rounds; ++i) {
        const auto &text = workloads[w][i % workloads[w].size()];
        ObFTWord word(text.size(), text.data(), meta);
        bool found = false;
        const int ret = fast ? checker.check_stopword(word, found) : oracle.check(word, found);
        require(ret == OB_SUCCESS, "stopword benchmark failed");
        hits += found;
      }
      const double cpu = thread_cpu_seconds() - cpu_start;
      if (expected_hits < 0) { expected_hits = hits; }
      require(hits == expected_hits, "stopword benchmark result changed");
      std::printf("STOPWORD workload=%s method=%s operations=%d cpu_sec=%.6f hits=%ld\n",
                  names[w], fast ? "indexed" : "legacy", rounds, cpu, hits);
    }
  }
}

static void benchmark_word_map_updates()
{
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  std::vector<std::string> tokens;
  tokens.reserve(1024);
  for (int i = 0; i < 1024; ++i) { tokens.push_back("frequency_word_" + std::to_string(i)); }
  constexpr int rounds = 500000;
  // Isolate the changed hash-map operation from SQL, network and JSON costs.
  // ABBA order and a thread CPU clock reduce sensitivity to host scheduling.
  for (const bool single_pass : {false, true, true, false}) {
    ObFTWordMap words;
    require(words.create(997, "FTWordBench") == OB_SUCCESS, "benchmark map init failed");
    auto increment = [](oceanbase::common::hash::HashMapPair<ObFTWord, int64_t> &entry) { ++entry.second; };
    const double cpu_start = thread_cpu_seconds();
    const auto wall_start = std::chrono::steady_clock::now();
    for (int i = 0; i < rounds; ++i) {
      const auto &token = tokens[i % tokens.size()];
      ObFTWord word(token.size(), token.data(), meta);
      if (single_pass) {
        require(words.set_or_update(word, 1, increment) == OB_SUCCESS, "single-pass update failed");
      } else {
        int64_t count = 0;
        const int ret = words.get_refactored(word, count);
        require(ret == OB_SUCCESS || ret == OB_HASH_NOT_EXIST, "legacy lookup failed");
        count = ret == OB_HASH_NOT_EXIST ? 1 : count + 1;
        require(words.set_refactored(word, count, 1) == OB_SUCCESS, "legacy update failed");
      }
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    const double cpu = thread_cpu_seconds() - cpu_start;
    int64_t total = 0;
    for (const auto &entry : words) { total += entry.second; }
    require(total == rounds && words.size() == 1024, "benchmark word frequencies changed");
    std::printf("MICRO %s operations=%d cpu_sec=%.6f wall_sec=%.6f total_frequency=%ld\n",
                single_pass ? "single_pass" : "legacy", rounds, cpu, wall, total);
  }
}

int main(int argc, char **argv)
{
  try {
    if (argc == 2 && std::strcmp(argv[1], "--benchmark-stopwords") == 0) {
      benchmark_stopwords();
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--test-stopwords") == 0) {
      test_stopword_equivalence();
      test_word_frequency_updates();
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--benchmark-word-map") == 0) {
      benchmark_word_map_updates();
      return 0;
    }
    test_character_classification();
    test_result_cell_lifetime();
    test_chain_lookup();
    test_beng_token_lifetime();
    test_range_dict_cleanup();
    test_word_frequency_updates();
    test_stopword_equivalence();
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1;
  }
  return 0;
}
