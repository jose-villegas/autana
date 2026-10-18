#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "asset/asset_table.h"
#include "asset/asset_table_format.h"
#include "test_pack.h"

enum {
    COLUMNS = 3,
    ROWS = 2,
    FRAC_BITS = 4,
    STRIDE = ASSET_TABLE_NAME_SIZE + COLUMNS * sizeof(int16_t),
    TABLE_BYTES = ASSET_TABLE_HEADER_SIZE + ROWS * STRIDE,
};

static uint8_t*
fixture(void) {
    uint8_t* bytes = calloc(TABLE_BYTES + 1, 1);
    TEST_ASSERT_NOT_NULL(bytes);
    test_pack_put16(bytes, ASSET_TABLE_VERSION);
    test_pack_put16(bytes + ASSET_TABLE_AT_ROWS, ROWS);
    test_pack_put16(bytes + ASSET_TABLE_AT_COLUMNS, COLUMNS);
    bytes[ASSET_TABLE_AT_FRAC_BITS] = FRAC_BITS;
    memcpy(bytes + ASSET_TABLE_HEADER_SIZE, "first", sizeof "first");
    memcpy(bytes + ASSET_TABLE_HEADER_SIZE + STRIDE, "second", sizeof "second");
    test_pack_put16(bytes + ASSET_TABLE_HEADER_SIZE + ASSET_TABLE_NAME_SIZE, (uint16_t)INT16_MIN);
    return bytes;
}

static void
test_rows_point_into_the_entry_and_lookup_checks_columns_and_names(void) {
    uint8_t* bytes = fixture();
    asset_table_t table;
    const asset_status_t opened = asset_table_open((asset_view_t){bytes, TABLE_BYTES}, &table);
    const int16_t* row = NULL;
    const asset_status_t found = asset_table_row(&table, "first", COLUMNS, &row);
    const int16_t* first_row = row;
    const bool in_place = (const void*)row == bytes + ASSET_TABLE_HEADER_SIZE + ASSET_TABLE_NAME_SIZE;
    const int value = row == NULL ? 0 : row[0];
    const asset_status_t mismatch = asset_table_row(&table, "first", COLUMNS + 1, &row);
    const bool cleared = row == NULL;
    row = first_row;
    const asset_status_t missing = asset_table_row(&table, "absent", COLUMNS, &row);
    const bool missing_cleared = row == NULL;
    row = first_row;
    const asset_status_t prefix = asset_table_row(&table, "fir", COLUMNS, &row);
    const bool prefix_cleared = row == NULL;
    row = first_row;
    const asset_status_t extended = asset_table_row(&table, "firstly", COLUMNS, &row);
    const bool extended_cleared = row == NULL;
    const asset_status_t second = asset_table_row(&table, "second", COLUMNS, &row);
    const bool second_in_place = (const void*)row == bytes + ASSET_TABLE_HEADER_SIZE + STRIDE + ASSET_TABLE_NAME_SIZE;
    free(bytes);
    TEST_ASSERT_EQUAL(ASSET_OK, opened);
    TEST_ASSERT_EQUAL(ASSET_OK, found);
    TEST_ASSERT_TRUE(in_place);
    TEST_ASSERT_EQUAL_INT(INT16_MIN, value);
    TEST_ASSERT_EQUAL_INT(ROWS, table.rows);
    TEST_ASSERT_EQUAL_INT(FRAC_BITS, table.frac_bits);
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, mismatch);
    TEST_ASSERT_TRUE(cleared);
    TEST_ASSERT_EQUAL(ASSET_ERR_NOT_FOUND, missing);
    TEST_ASSERT_TRUE(missing_cleared);
    TEST_ASSERT_EQUAL(ASSET_ERR_NOT_FOUND, prefix);
    TEST_ASSERT_TRUE(prefix_cleared);
    TEST_ASSERT_EQUAL(ASSET_ERR_NOT_FOUND, extended);
    TEST_ASSERT_TRUE(extended_cleared);
    TEST_ASSERT_EQUAL(ASSET_OK, second);
    TEST_ASSERT_TRUE(second_in_place);
}

static void
test_short_misaligned_wrong_version_and_wrong_size_entries_are_rejected(void) {
    uint8_t* bytes = fixture();
    asset_table_t table;
    const asset_status_t short_header = asset_table_open((asset_view_t){bytes, ASSET_TABLE_HEADER_SIZE - 1}, &table);
    const asset_status_t short_entry = asset_table_open((asset_view_t){bytes, TABLE_BYTES - 1}, &table);
    const asset_status_t long_entry = asset_table_open((asset_view_t){bytes, TABLE_BYTES + 1}, &table);
    test_pack_put16(bytes, ASSET_TABLE_VERSION + 1);
    const asset_status_t version = asset_table_open((asset_view_t){bytes, TABLE_BYTES}, &table);
    memmove(bytes + 1, bytes, TABLE_BYTES);
    const asset_status_t alignment = asset_table_open((asset_view_t){bytes + 1, TABLE_BYTES}, &table);
    free(bytes);
    TEST_ASSERT_EQUAL(ASSET_ERR_BOUNDS, short_header);
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, short_entry);
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, long_entry);
    TEST_ASSERT_EQUAL(ASSET_ERR_VERSION, version);
    TEST_ASSERT_EQUAL(ASSET_ERR_BOUNDS, alignment);
}

static void
test_empty_dimensions_reserved_byte_and_unterminated_names_are_rejected(void) {
    static const int fields[] = {ASSET_TABLE_AT_ROWS, ASSET_TABLE_AT_COLUMNS, ASSET_TABLE_AT_RESERVED};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        uint8_t* bytes = fixture();
        bytes[fields[i]] = fields[i] == ASSET_TABLE_AT_RESERVED ? 1 : 0;
        asset_table_t table;
        const asset_status_t status = asset_table_open((asset_view_t){bytes, TABLE_BYTES}, &table);
        free(bytes);
        TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, status);
        TEST_ASSERT_NULL(table.base);
    }
    uint8_t* bytes = fixture();
    memset(bytes + ASSET_TABLE_HEADER_SIZE + STRIDE, 'x', ASSET_TABLE_NAME_SIZE);
    asset_table_t table;
    const asset_status_t status = asset_table_open((asset_view_t){bytes, TABLE_BYTES}, &table);
    free(bytes);
    TEST_ASSERT_EQUAL(ASSET_ERR_FORMAT, status);
}

void
suite_asset_table(void) {
    RUN_TEST(test_rows_point_into_the_entry_and_lookup_checks_columns_and_names);
    RUN_TEST(test_short_misaligned_wrong_version_and_wrong_size_entries_are_rejected);
    RUN_TEST(test_empty_dimensions_reserved_byte_and_unterminated_names_are_rejected);
}

SUITE_REGISTER(suite_asset_table);
