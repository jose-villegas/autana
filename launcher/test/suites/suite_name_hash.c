#include <stddef.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "core/name_hash.h"
#include "fixtures/name_hash_vectors.h"

static const char* const names[] = NAME_HASH_TEST_NAMES;
static const name_hash_t hashes[] = NAME_HASH_TEST_HASHES;

static void
test_name_hash_shared_vectors(void) {
    const size_t count = sizeof names / sizeof names[0];
    TEST_ASSERT_EQUAL_UINT(count, sizeof hashes / sizeof hashes[0]);
    for (size_t i = 0; i < count; i++) {
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

void
run_name_hash_suite(void) {
    RUN_TEST(test_name_hash_shared_vectors);
    RUN_TEST(test_name_hash_embedded_nul);
}

SUITE_REGISTER(run_name_hash_suite);
