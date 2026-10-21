/* The opening vectors are published FNV-1a references; all consumers share this table. */
#pragma once

// clang-format off
#define NAME_HASH_TEST_NAMES {"", "a", "foobar", "camera", ".", "position", "half_fov_short_tan", "café"}
#define NAME_HASH_TEST_HASHES {0x811C9DC5U, 0xE40C292CU, 0xBF9CF968U, 0x9EBE984EU, 0x2B0C98F1U, 0x934F4E0AU, 0x4ECDDE39U, 0xA82B5049U}
#define NAME_HASH_TEST_BYTES "a\0foobar"
#define NAME_HASH_TEST_BYTES_HASH 0xFD337D79U
// clang-format on
