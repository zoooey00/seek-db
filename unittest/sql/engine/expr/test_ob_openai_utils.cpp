/*
 * Copyright (c) 2025 OceanBase.
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

#define USING_LOG_PREFIX SQL_ENG
#include <gtest/gtest.h>
#include <limits>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace
{
ObJsonObject *parse(ObIAllocator &allocator, const char *text)
{
  ObString str(text);
  ObJsonObject *object = nullptr;
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, str, object));
  return object;
}
}

TEST(AIProvider, OpenAIEmbeddingRestoresIndexOrder)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *result = nullptr;
  auto response = parse(allocator, R"({"data":[{"index":1,"embedding":[2,3]},{"index":0,"embedding":[0,1]}]})");
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator, response, result));
  ASSERT_NE(nullptr, result);
  EXPECT_EQ(2, result->element_count());
  EXPECT_EQ(0, static_cast<ObJsonArray *>(static_cast<ObJsonArray *>(result)->get_value(0))->get_value(0)->get_int());
}

TEST(AIProvider, OpenAIEmbeddingRejectsMalformedResponses)
{
  const char *invalid[] = {
    R"({})", R"({"data":null})", R"({"data":{}})", R"({"data":[]})",
    R"({"data":[0]})", R"({"data":[{"index":0}]})",
    R"({"data":[{"embedding":[1]}]})",
    R"({"data":[{"index":-1,"embedding":[1]}]})",
    R"({"data":[{"index":1,"embedding":[1]}]})",
    R"({"data":[{"index":0.5,"embedding":[1]}]})",
    R"({"data":[{"index":0,"embedding":null}]})",
    R"({"data":[{"index":0,"embedding":[]}]})",
    R"({"data":[{"index":0,"embedding":["1"]}]})",
    R"({"data":[{"index":0,"embedding":[true]}]})",
    R"({"data":[{"index":0,"embedding":[null]}]})",
    R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})",
    R"({"data":[{"index":0,"embedding":[1]},{"index":1,"embedding":[2,3]}]})"
  };
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  for (auto text : invalid) {
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, parse(allocator, text), result)) << text;
    EXPECT_EQ(nullptr, result);
  }
}

TEST(AIProvider, EmbeddingChecksDimensionCountAndFiniteValues)
{
  ObArenaAllocator allocator;
  auto object = parse(allocator, R"({"vectors":[[1,2],[3,4]]})");
  auto value = object->get_value("vectors");
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator, value, 2, 2));
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator, value, 2, 0));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator, value, 1, 2));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator, value, 2, 3));
  ObJsonDouble bad(std::numeric_limits<double>::infinity());
  static_cast<ObJsonArray *>(static_cast<ObJsonArray *>(value)->get_value(0))->append(&bad);
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator, value, 2, 0));
}

TEST(AIProvider, CompletionRejectsMissingEmptyOrNonStringContent)
{
  const char *invalid[] = {R"({})", R"({"choices":[]})",
    R"({"choices":[{"message":{"content":""}}]})",
    R"({"choices":[{"message":{"content":null}}]})",
    R"({"choices":[{"message":{"content":[1]}}]})"};
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIComplete provider;
  for (auto text : invalid) {
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, parse(allocator, text), result)) << text;
  }
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator,
      parse(allocator, R"({"choices":[{"message":{"content":"hello 世界"}}]})"), result));
  EXPECT_EQ(ObString("hello 世界"), static_cast<ObJsonString *>(result)->get_str());
}

TEST(AIProvider, OpenAICompletionPassesConfig)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIComplete provider;
  ObString model("any-model"), system(""), prompt("hello");
  ObJsonObject *body = nullptr;
  auto config = parse(allocator, R"({"temperature":0.25,"max_tokens":7})");
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, system, prompt, config, body));
  EXPECT_NE(nullptr, body->get_value("temperature"));
  EXPECT_EQ(7, body->get_value("max_tokens")->get_int());
  EXPECT_EQ(1, body->get_value("messages")->element_count());
}

TEST(AIProvider, OllamaUsesNativeNonStreamingCompletionAndNoKey)
{
  ObArenaAllocator allocator;
  ObOllamaUtils::ObOllamaComplete provider;
  ObString key(""), model("arbitrary"), system("system"), prompt("hello");
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, provider.get_header(allocator, key, headers));
  ASSERT_EQ(1, headers.count());
  EXPECT_EQ(ObString("Content-Type: application/json"), headers.at(0));
  ObJsonObject *body = nullptr;
  auto config = parse(allocator, R"({"temperature":0.5,"max_tokens":12,"stream":true})");
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, system, prompt, config, body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(static_cast<ObJsonBoolean *>(body->get_value("stream"))->value());
  ASSERT_EQ(ObJsonNodeType::J_OBJECT, body->get_value("options")->json_type());
  auto options = static_cast<ObJsonObject *>(body->get_value("options"));
  EXPECT_EQ(12, options->get_value("num_predict")->get_int());
  EXPECT_EQ(ObString("system"), static_cast<ObJsonString *>(body->get_value("system"))->get_str());
}

TEST(AIProvider, OllamaFactoriesIgnoreProviderCase)
{
  ObArenaAllocator allocator;
  ObAIFuncIComplete *completion = nullptr;
  ObAIFuncIEmbed *embed = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator, ObString("oLlAmA"), completion));
  ASSERT_NE(nullptr, completion);
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator, ObString("OLLAMA"), embed));
  ASSERT_NE(nullptr, embed);
}

TEST(AIProvider, OllamaEmbeddingAndResponseValidation)
{
  ObArenaAllocator allocator;
  ObOllamaUtils::ObOllamaEmbed provider;
  ObString model("arbitrary");
  ObArray<ObString> inputs;
  inputs.push_back(ObString("one"));
  inputs.push_back(ObString("two"));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, inputs, nullptr, body));
  EXPECT_EQ(2, body->get_value("input")->element_count());
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator, parse(allocator, R"({"embeddings":[[1,2],[3,4]]})"), result));
  EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, parse(allocator, R"({"embeddings":{}})"), result));
  EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, parse(allocator, R"({"embeddings":[[1,"x"]]})"), result));
}

TEST(AIProvider, RerankSortsAndPreservesStableIndices)
{
  ObArenaAllocator allocator;
  auto response = parse(allocator, R"({"results":[{"index":2,"relevance_score":0.5},{"index":1,"relevance_score":1},{"index":0,"relevance_score":0.5}]})");
  ObJsonArray *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::normalize_rerank_result(allocator, response->get_value("results"), 3, result));
  EXPECT_EQ(1, static_cast<ObJsonObject *>(result->get_value(0))->get_value("index")->get_int());
  EXPECT_EQ(0, static_cast<ObJsonObject *>(result->get_value(1))->get_value("index")->get_int());
  EXPECT_EQ(2, static_cast<ObJsonObject *>(result->get_value(2))->get_value("index")->get_int());
}

TEST(AIProvider, RerankRejectsInvalidCountIndexAndScore)
{
  const char *invalid[] = {R"({"results":[]})", R"({"results":[null]})",
    R"({"results":[{"index":-1,"relevance_score":1}]})",
    R"({"results":[{"index":1,"relevance_score":1}]})",
    R"({"results":[{"index":0,"relevance_score":"1"}]})",
    R"({"results":[{"index":0}]})"};
  ObArenaAllocator allocator;
  for (auto text : invalid) {
    ObJsonArray *result = nullptr;
    auto response = parse(allocator, text);
    EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::normalize_rerank_result(allocator, response->get_value("results"), 1, result)) << text;
  }
  ObJsonArray *result = nullptr;
  auto response = parse(allocator, R"({"results":[{"index":0,"relevance_score":1},{"index":0,"relevance_score":1}]})");
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::normalize_rerank_result(allocator, response->get_value("results"), 2, result));
}

TEST(AIFuncJsonUtils, NullPrintIsRejected)
{
  ObArenaAllocator allocator;
  ObString output;
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncJsonUtils::print_json_to_str(allocator, nullptr, output));
}

TEST(AIFuncJsonUtils, NonObjectRootIsRejected)
{
  ObArenaAllocator allocator;
  for (const char *text : {"[]", "null", "1", "\"string\""}) {
    ObString input(text);
    ObJsonObject *object = nullptr;
    EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, object));
    EXPECT_EQ(nullptr, object);
  }
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
