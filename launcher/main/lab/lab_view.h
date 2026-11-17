/*
 * lab_view: the view tunables every lab has, declared once here and
 * registered per lab under its name: "<name>.scale", "<name>.present",
 * "<name>.layout" and "<name>.orbit_ms".
 */
#pragma once

#include <stdint.h>

#include "core/tune.h"
#include "scene/camera/camera.h"

#define LAB_SCALE_UNIT    100
#define LAB_SCALE_DEFAULT (2 * LAB_SCALE_UNIT)
#define LAB_SCALE_MAX     (8 * LAB_SCALE_UNIT)

/* The slowest orbit_ms turn: one a minute. */
#define LAB_ORBIT_MAX_MS  60000

/* The framebuffer layout a lab asks for when it enters a scene. */
typedef enum {
    LAB_LAYOUT_FULL,
    LAB_LAYOUT_DOUBLE,
    LAB_LAYOUT_DOUBLE_CLEAR, /* double, the sent frame filled by core 1 */
    LAB_LAYOUT_COUNT,
} lab_layout_t;

typedef enum {
    LAB_VIEW_SCALE,    /* in hundredths: 200 renders at half size */
    LAB_VIEW_PRESENT,  /* a camera_present_t */
    LAB_VIEW_LAYOUT,   /* a lab_layout_t, taken at the next enter or scene switch */
    LAB_VIEW_ORBIT_MS, /* 0 holds an orbiting camera where it is steered; otherwise
                        * it also turns once about its subject every orbit_ms */
    LAB_VIEW_COUNT,
} lab_view_tunable_t;

/* One lab's view tunables: their values, and where TUNE_ENABLED, the entries
 * and names the console SETs them through. */
typedef struct {
    int32_t value[LAB_VIEW_COUNT];
#if TUNE_ENABLED
    tune_owner_t owner;
    tune_entry_t entry[LAB_VIEW_COUNT];
    char name[LAB_VIEW_COUNT][TUNE_NAME_MAX + 1];
#endif
} lab_view_t;

/* Sets every tunable to its default and, where TUNE_ENABLED, registers them
 * as "<name>.<tunable>", once, before the console runs (LAB_VIEW()). */
void lab_view_register(lab_view_t* view, const char* name);

/* Defines a lab's view tunables and registers them as "<name>.<tunable>"
 * before app_main(): LAB_VIEW(my_view, "my_lab") owns my_lab.scale ... */
#define LAB_VIEW(symbol, name)                                                                                         \
    lab_view_t symbol;                                                                                                 \
    __attribute__((constructor)) static void symbol##_register(void) { lab_view_register(&symbol, name); }

int lab_view_scale(const lab_view_t* view);
camera_present_t lab_view_present(const lab_view_t* view);
lab_layout_t lab_view_layout(const lab_view_t* view);
int lab_view_orbit_ms(const lab_view_t* view);
