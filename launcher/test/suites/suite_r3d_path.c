/*
 * Host suite: r3d_path, the renderer's fill-path counters a host run reads
 * to name the paths no stress preset reaches.
 */

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "render/path/r3d_path.h"

#if R3D_PATHS

R3D_PATH(probe, "test.probe");

static const r3d_path_t*
find(const char* name) {
    for (const r3d_path_t* p = r3d_path_first(); p != NULL; p = p->next) {
        if (strcmp(p->name, name) == 0) {
            return p;
        }
    }
    return NULL;
}

/* A path joins the list when the program starts, before anything takes it:
 * a path nothing reached is still there to be named. */
static void
test_a_declared_path_is_listed_before_anything_takes_it(void) {
    const r3d_path_t* probe = find("test.probe");
    TEST_ASSERT_NOT_NULL(probe);
    TEST_ASSERT_NOT_NULL(find("span.walk.gouraud"));
    TEST_ASSERT_NOT_NULL(find("texture.affine"));
}

/* Names in order, and a name's copies (a test including a source file holds
 * a second) neighbours from one source, so a reader sums each name's run. */
static void
test_the_list_is_in_name_order_and_copies_are_neighbours(void) {
    for (const r3d_path_t* p = r3d_path_first(); p != NULL && p->next != NULL; p = p->next) {
        const int order = strcmp(p->name, p->next->name);
        TEST_ASSERT_TRUE_MESSAGE(order <= 0, p->next->name);
        if (order == 0) {
            TEST_ASSERT_EQUAL_STRING(p->file, p->next->file);
        }
    }
}

static void
test_each_hit_counts_and_forget_zeroes_them(void) {
    r3d_path_forget();
    R3D_PATH_HIT(probe);
    R3D_PATH_HIT(probe);
    TEST_ASSERT_EQUAL_UINT64(2, find("test.probe")->hits);
    r3d_path_forget();
    TEST_ASSERT_EQUAL_UINT64(0, find("test.probe")->hits);
}

#endif

void
suite_r3d_path(void) {
#if R3D_PATHS
    RUN_TEST(test_a_declared_path_is_listed_before_anything_takes_it);
    RUN_TEST(test_the_list_is_in_name_order_and_copies_are_neighbours);
    RUN_TEST(test_each_hit_counts_and_forget_zeroes_them);
#endif
}

SUITE_REGISTER(suite_r3d_path);
