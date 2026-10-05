/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#include "velox/common/base/tests/GTestUtils.h"
#include "velox/functions/sparksql/tests/SparkFunctionBaseTest.h"

namespace facebook::velox::functions::sparksql::test {
namespace {

using namespace facebook::velox::test;

class TransformKeysTest : public SparkFunctionBaseTest {};

TEST_F(TransformKeysTest, basic) {
  auto data = makeRowVector({
      makeMapVectorFromJson<int64_t, int64_t>({
          "{1: 10, 2: 20, 3: 30}",
          "{}",
          "null",
          "{4: 40}",
      }),
  });

  for (const bool throwOnDuplicateKeys : {false, true}) {
    SCOPED_TRACE(throwOnDuplicateKeys ? "EXCEPTION" : "LAST_WIN");
    setThrowExceptionOnDuplicateMapKeys(throwOnDuplicateKeys);

    auto expected = makeMapVectorFromJson<int64_t, int64_t>({
        "{11: 10, 12: 20, 13: 30}",
        "{}",
        "null",
        "{14: 40}",
    });
    assertEqualVectors(
        expected, evaluate("transform_keys(c0, (k, v) -> add(k, 10))", data));

    expected = makeMapVectorFromJson<int64_t, int64_t>({
        "{11: 10, 22: 20, 33: 30}",
        "{}",
        "null",
        "{44: 40}",
    });
    assertEqualVectors(
        expected, evaluate("transform_keys(c0, (k, v) -> add(k, v))", data));
  }
}

TEST_F(TransformKeysTest, lastWinDuplicateKeys) {
  setThrowExceptionOnDuplicateMapKeys(false);

  auto data = makeRowVector({
      makeMapVector<int64_t, int64_t>({
          {{1, 10}, {2, 20}, {3, 30}},
          {{1, 10}, {2, 20}, {3, 30}, {4, 40}},
          {{5, 50}},
          {{2, 20}, {4, 40}, {6, 60}},
      }),
  });

  // A repeated key keeps the position of its first occurrence and takes the
  // value of its last one. The assertions read map_keys and map_values because
  // map equality ignores entry order.
  assertEqualVectors(
      makeArrayVectorFromJson<int64_t>({
          "[1, 0]",
          "[1, 0]",
          "[1]",
          "[0]",
      }),
      evaluate(
          "map_keys(transform_keys(c0, (k, v) -> remainder(k, 2)))", data));
  assertEqualVectors(
      makeArrayVectorFromJson<int64_t>({
          "[30, 20]",
          "[30, 40]",
          "[50]",
          "[60]",
      }),
      evaluate(
          "map_values(transform_keys(c0, (k, v) -> remainder(k, 2)))", data));
}

TEST_F(TransformKeysTest, throwOnDuplicateKeys) {
  setThrowExceptionOnDuplicateMapKeys(true);

  auto data = makeRowVector({
      makeMapVector<int64_t, int64_t>({
          {{1, 10}, {2, 20}, {3, 30}},
          {{5, 50}},
      }),
  });

  VELOX_ASSERT_USER_THROW(
      evaluate("transform_keys(c0, (k, v) -> remainder(k, 2))", data),
      "Duplicate map key (1) was found.");

  auto expected = makeMapVectorFromJson<int64_t, int64_t>({
      "null",
      "{1: 50}",
  });
  assertEqualVectors(
      expected,
      evaluate("try(transform_keys(c0, (k, v) -> remainder(k, 2)))", data));
}

TEST_F(TransformKeysTest, nullKey) {
  auto data = makeRowVector({
      makeMapVectorFromJson<int64_t, int64_t>({
          "{1: 10, 2: 20}",
          "{3: 30}",
      }),
      makeNullableFlatVector<int64_t>({std::nullopt, 7}),
  });

  for (const bool throwOnDuplicateKeys : {false, true}) {
    SCOPED_TRACE(throwOnDuplicateKeys ? "EXCEPTION" : "LAST_WIN");
    setThrowExceptionOnDuplicateMapKeys(throwOnDuplicateKeys);

    VELOX_ASSERT_USER_THROW(
        evaluate("transform_keys(c0, (k, v) -> c1)", data),
        "Cannot use null as map key!");

    auto expected = makeMapVectorFromJson<int64_t, int64_t>({
        "null",
        "{7: 30}",
    });
    assertEqualVectors(
        expected, evaluate("try(transform_keys(c0, (k, v) -> c1))", data));
  }
}

TEST_F(TransformKeysTest, arrayKeysWithNestedNulls) {
  auto data = makeRowVector({
      makeMapVector<int64_t, int64_t>({
          {{1, 10}, {2, 20}},
          {{3, 30}},
      }),
      makeArrayVectorFromJson<int64_t>({
          "[1, null]",
          "[2, null]",
      }),
  });

  setThrowExceptionOnDuplicateMapKeys(false);
  auto expected = makeMapVector(
      {0, 1},
      makeArrayVectorFromJson<int64_t>({
          "[1, null]",
          "[2, null]",
      }),
      makeFlatVector<int64_t>({20, 30}));
  assertEqualVectors(
      expected, evaluate("transform_keys(c0, (k, v) -> c1)", data));

  setThrowExceptionOnDuplicateMapKeys(true);
  VELOX_ASSERT_USER_THROW(
      evaluate("transform_keys(c0, (k, v) -> c1)", data),
      "Duplicate map key ({1, null}) was found.");

  expected = makeMapVector(
      {0, 0},
      makeArrayVectorFromJson<int64_t>({"[2, null]"}),
      makeFlatVector<int64_t>({30}));
  expected->setNull(0, true);
  assertEqualVectors(
      expected, evaluate("try(transform_keys(c0, (k, v) -> c1))", data));
}

TEST_F(TransformKeysTest, dictionaryEncodedInput) {
  setThrowExceptionOnDuplicateMapKeys(false);

  auto baseMaps = makeMapVector<int64_t, int64_t>({
      {{1, 10}, {2, 20}, {3, 30}},
      {{4, 40}, {5, 50}},
  });
  // Rows are read out of order and the second base row is read twice, so the
  // result offsets must follow the dictionary rather than the base vector. The
  // flat capture keeps the dictionary from being peeled off before the function
  // runs.
  auto data = makeRowVector({
      wrapInDictionary(makeIndices({1, 0, 1}), 3, baseMaps),
      makeFlatVector<int64_t>({10, 20, 30}),
  });

  auto expected = makeMapVectorFromJson<int64_t, int64_t>({
      "{0: 40, 1: 50}",
      "{1: 30, 0: 20}",
      "{0: 40, 1: 50}",
  });
  assertEqualVectors(
      expected,
      evaluate("transform_keys(c0, (k, v) -> remainder(add(k, c1), 2))", data));
}

TEST_F(TransformKeysTest, conditional) {
  setThrowExceptionOnDuplicateMapKeys(false);

  // Each branch writes a disjoint subset of rows into the same result vector.
  auto data = makeRowVector({
      makeMapVector<int64_t, int64_t>({
          {{1, 10}, {2, 20}, {3, 30}},
          {{1, 10}, {2, 20}, {3, 30}},
          {{4, 40}},
      }),
      makeFlatVector<int64_t>({1, 2, 3}),
  });

  auto expected = makeMapVectorFromJson<int64_t, int64_t>({
      "{11: 10, 12: 20, 13: 30}",
      "{1: 30, 0: 20}",
      "{0: 40}",
  });
  assertEqualVectors(
      expected,
      evaluate(
          "if(greaterthan(c1, 1), transform_keys(c0, (k, v) -> remainder(k, 2)), "
          "transform_keys(c0, (k, v) -> add(k, 10)))",
          data));
}

} // namespace
} // namespace facebook::velox::functions::sparksql::test
