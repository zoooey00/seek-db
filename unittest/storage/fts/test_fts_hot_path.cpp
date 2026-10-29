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

int main()
{
  try {
    test_character_classification();
    test_result_cell_lifetime();
    test_chain_lookup();
    test_beng_token_lifetime();
    test_range_dict_cleanup();
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1;
  }
  return 0;
}
