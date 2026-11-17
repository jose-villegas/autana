#include "lab/lab_view.h"

#include <assert.h>
#include <stdio.h>

/* Each tunable's name after the lab's, its default and its range. */
typedef struct {
    const char* what;
    int32_t initial, low, high;
} lab_view_spec_t;

static const lab_view_spec_t specs[LAB_VIEW_COUNT] = {
    [LAB_VIEW_SCALE] = {"scale", LAB_SCALE_DEFAULT, LAB_SCALE_UNIT, LAB_SCALE_MAX},
    [LAB_VIEW_PRESENT] = {"present", CAMERA_PRESENT_NEAREST, CAMERA_PRESENT_NEAREST, CAMERA_PRESENT_DOUBLE},
    [LAB_VIEW_LAYOUT] = {"layout", LAB_LAYOUT_FULL, LAB_LAYOUT_FULL, LAB_LAYOUT_COUNT - 1},
    [LAB_VIEW_ORBIT_MS] = {"orbit_ms", 0, 0, LAB_ORBIT_MAX_MS},
};

void
lab_view_register(lab_view_t* view, const char* name) {
    for (int i = 0; i < LAB_VIEW_COUNT; i++) {
        view->value[i] = specs[i].initial;
#if TUNE_ENABLED
        const int length = snprintf(view->name[i], sizeof view->name[i], "%s.%s", name, specs[i].what);
        assert(length > 0 && length <= TUNE_NAME_MAX);
        (void)length;
        view->entry[i] = (tune_entry_t){
            view->name[i], &view->value[i], specs[i].initial, specs[i].low, specs[i].high, &view->owner, NULL};
        (void)tune_register(tune_shared(), &view->entry[i]);
#else
        (void)name;
#endif
    }
}

int
lab_view_scale(const lab_view_t* view) {
    return view->value[LAB_VIEW_SCALE];
}

camera_present_t
lab_view_present(const lab_view_t* view) {
    return (camera_present_t)view->value[LAB_VIEW_PRESENT];
}

lab_layout_t
lab_view_layout(const lab_view_t* view) {
    return (lab_layout_t)view->value[LAB_VIEW_LAYOUT];
}

int
lab_view_orbit_ms(const lab_view_t* view) {
    return view->value[LAB_VIEW_ORBIT_MS];
}
