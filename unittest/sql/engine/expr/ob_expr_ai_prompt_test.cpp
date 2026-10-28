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
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace
{
int render(ObIAllocator &allocator, const char *json, ObString &result)
{
  ObString input(json);
  ObJsonObject *object = nullptr;
  int ret = ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, object);
  return ret == OB_SUCCESS ? ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, object, result) : ret;
}
}

TEST(AIPrompt, RepeatedPlaceholdersAndUnicodeEscapes)
{
  ObArenaAllocator allocator;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(allocator, R"({"template":"{0}/{0}/{0}/{0}\n{1}","args":["你好世界","\"\\"]})", result));
  EXPECT_EQ(ObString("你好世界/你好世界/你好世界/你好世界\n\"\\"), result);
}

TEST(AIPrompt, JsonArraysObjectsAndNestedPrompt)
{
  ObArenaAllocator allocator;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(allocator,
      R"({"template":"{0}:{1}:{2}","args":[[1,2],{"a":"中"},{"template":"nested {0}","args":["ok"]}]})", result));
  std::string text(result.ptr(), result.length());
  EXPECT_NE(std::string::npos, text.find("[1, 2]"));
  EXPECT_NE(std::string::npos, text.find("\"a\""));
  EXPECT_NE(std::string::npos, text.find("nested ok"));
}

TEST(AIPrompt, InvalidAndOverflowingIndices)
{
  const char *invalid[] = {
    R"({"template":"{1}","args":["x"]})",
    R"({"template":"{999999999999999999999999999999999999999999}","args":["x"]})",
    R"({"template":12,"args":["x"]})",
    R"({"template":"{0}","args":{}})"
  };
  ObArenaAllocator allocator;
  for (auto json : invalid) {
    ObString result;
    EXPECT_NE(OB_SUCCESS, render(allocator, json, result)) << json;
  }
}

TEST(AIPrompt, LongRepeatedArgumentsDoNotUnderallocate)
{
  ObArenaAllocator allocator;
  std::string argument(100000, 'x');
  std::string json = "{\"template\":\"{0}{0}{0}{0}\",\"args\":[\"" + argument + "\"]}";
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(allocator, json.c_str(), result));
  EXPECT_EQ(400000, result.length());
}

TEST(AIPrompt, InsertedPlaceholdersAreNotReinterpreted)
{
  ObArenaAllocator allocator;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(allocator, R"({"template":"{0}-{1}","args":["{1}","ok"]})", result));
  EXPECT_EQ(ObString("{1}-ok"), result);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
