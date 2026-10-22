#include <stddef.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "name_hash/name_hash.h"
#include "name_hash_vectors.h"

static const char* const names[] = NAME_HASH_TEST_NAMES;
static const name_hash_t hashes[] = NAME_HASH_TEST_HASHES;
static const name_hash64_t hashes64[] = NAME_HASH64_TEST_HASHES;

static void
test_name_hash_shared_vectors(void) {
    const size_t count = sizeof names / sizeof names[0];
    TEST_ASSERT_EQUAL_UINT(count, sizeof hashes / sizeof hashes[0]);
    TEST_ASSERT_EQUAL_UINT(count, sizeof hashes64 / sizeof hashes64[0]);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_HEX64(hashes64[i], name_hash64_n(NAME_HASH64_OFFSET_BASIS, names[i], strlen(names[i])));
        const name_hash_t actual = name_hash(names[i]);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(hashes[i], actual, names[i]);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(NAME_HASH_NONE, actual, names[i]);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(hashes[i], name_hash_n(names[i], strlen(names[i])), names[i]);
    }
}

static void
test_name_hash_embedded_nul(void) {
    TEST_ASSERT_EQUAL_HEX32(NAME_HASH_TEST_BYTES_HASH,
                            name_hash_n(NAME_HASH_TEST_BYTES, sizeof NAME_HASH_TEST_BYTES - 1));
}

static void
test_name_hash64_shared_vectors(void) {
    TEST_ASSERT_EQUAL_HEX64(NAME_HASH64_TEST_BYTES_HASH, name_hash64_n(NAME_HASH64_OFFSET_BASIS, NAME_HASH_TEST_BYTES,
                                                                       sizeof NAME_HASH_TEST_BYTES - 1));
    const name_hash64_t prefix =
        name_hash64_n(NAME_HASH64_OFFSET_BASIS, NAME_HASH64_TEST_PREFIX, sizeof NAME_HASH64_TEST_PREFIX - 1);
    TEST_ASSERT_EQUAL_HEX64(NAME_HASH64_TEST_PREFIX_HASH, prefix);
    const name_hash64_t continued = name_hash64_n(prefix, NAME_HASH64_TEST_SUFFIX, sizeof NAME_HASH64_TEST_SUFFIX - 1);
    TEST_ASSERT_EQUAL_HEX64(NAME_HASH64_TEST_JOINED_HASH, continued);
    TEST_ASSERT_EQUAL_HEX64(name_hash64_n(NAME_HASH64_OFFSET_BASIS, NAME_HASH64_TEST_PREFIX NAME_HASH64_TEST_SUFFIX,
                                          sizeof NAME_HASH64_TEST_PREFIX NAME_HASH64_TEST_SUFFIX - 1),
                            continued);
    TEST_ASSERT_EQUAL_HEX64(prefix, name_hash64_n(prefix, NULL, 0));
}

void
run_name_hash_suite(void) {
    RUN_TEST(test_name_hash_shared_vectors);
    RUN_TEST(test_name_hash_embedded_nul);
    RUN_TEST(test_name_hash64_shared_vectors);
}

SUITE_REGISTER(run_name_hash_suite);
