/*
 * lab: a set of scenes behind one HUD and one BOOT menu, for an app that
 * shows them one at a time. The lab enters and leaves its scenes, switches
 * between them from the menu or the console, gives each the app arena from
 * its entry on, and draws the scene title, fps and status over it.
 *
 * The app owns one lab_t, hands it its scenes and any hooks, registers it
 * once (LAB_REGISTER()), which provides its app_t callbacks. Every hook is optional, so the
 * least a lab needs is its view tunables, its console word and its scenes:
 *
 *     LAB_VIEW(my_view, "my_lab");
 *     lab_t my_lab = {.view = &my_view, .console_word = "my", .scenes = scenes, .scene_count = 2};
 *     LAB_REGISTER(my_lab);
 *     app_t my_app = {.enter = my_lab_enter, .frame = my_lab_frame,
 *                     .update = my_lab_update, .exit = my_lab_exit};
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/app.h"
#include "input/input.h"
#include "lab/lab_scene.h"
#include "lab/lab_view.h"

/* The longest console word, its NUL included. */
#define LAB_WORD_MAX 16

/* What the app adds around the lab's own steps; every hook is optional. */
typedef struct {
    /* After a scene's enter(), and after each frame() it draws. */
    void (*scene_settle)(void);
    /* Before a scene's exit(). */
    void (*scene_leaving)(void);
    /* Before a scene's frame(). */
    void (*frame_begin)(const lab_scene_t* scene);
    /* After the scene's steer() and update(), menu open or not. */
    void (*update_end)(uint32_t dt_ms);
    /* The words after the console word that the lab's own commands did not
     * take, before the scene's console; false passes them on. */
    bool (*console)(const char* args);
    /* Suites only: just before a test frame renders, so a setting a test
     * changed reaches that frame (lab_test.h). */
    void (*test_render)(void);
    /* The lab's own menu rows, under the scene's. */
    lab_controls_fn settings;
} lab_hooks_t;

/* The lab's running state: its own, never set by the app. */
typedef struct {
    int scene;           /* index into scenes */
    int requested;       /* a scene asked for, switched to at the next frame; -1 for none */
    bool menu_open;      /* the BOOT menu shows in place of the scene */
    size_t scene_mark;   /* the app arena's use before the scene was entered */
    uint32_t title_ms;   /* how long the scene title still shows */
    uint32_t layout_gen; /* ui_layout_generation() at the last frame */
    uint32_t fps_frames; /* frames in the fps window so far */
    uint32_t fps_window_ms;
    float fps;
} lab_state_t;

typedef struct {
    /* The app's console word ("my"), named in lab_console()'s usage
     * replies; in capitals it prefixes them ("MY ..." lines). */
    const char* console_word;
    /* console_word in capitals, set by LAB_REGISTER(). */
    char reply_tag[LAB_WORD_MAX];
    /* Index 0 is the scene a start with no key shows; a family's scenes keep
     * this order in its dropdown. */
    const lab_scene_t* const* scenes;
    int scene_count;
    lab_hooks_t hooks;
    /* The scene lab_enter() shows; NULL or unknown is scenes[0]. A switch
     * writes it, so a later entry shows the same scene. */
    const char* start_key;
    /* The camera a scene activates on entry; NULL is its first. */
    const char* start_camera;
    /* Draws the HUD. A host render pinned to the scene alone turns it off:
     * the fps line is a float, formatted alike on no two compilers. */
    bool show_hud;
    /* The app the suites' fixtures enter (lab_test.h); NULL for a lab no suite enters. */
    const app_t* app;
    /* The view tunables (LAB_VIEW()). */
    lab_view_t* view;
    lab_state_t state;
} lab_t;

/* Sets the lab's reply prefix, once, before the console runs. */
void lab_register(lab_t* lab);

/* Registers a static lab_t and defines its app_t callbacks. */
#define LAB_REGISTER(symbol)                                                                                           \
    __attribute__((constructor)) static void symbol##_lab_register(void) { lab_register(&symbol); }                    \
    static void symbol##_enter(void) { lab_enter(&symbol); }                                                           \
    static void symbol##_frame(uint32_t dt_ms, const input_t* input) { lab_frame(&symbol, dt_ms, input); }             \
    static void symbol##_update(uint32_t dt_ms, const input_t* input) { lab_update(&symbol, dt_ms, input); }           \
    static void symbol##_exit(void) { lab_exit(&symbol); }

void lab_enter(lab_t* lab);
void lab_update(lab_t* lab, uint32_t dt_ms, const input_t* input);
void lab_frame(lab_t* lab, uint32_t dt_ms, const input_t* input);
void lab_exit(lab_t* lab);

const lab_scene_t* lab_current(const lab_t* lab);

/* Whether the BOOT menu shows in place of the scene. */
bool lab_menu_open(const lab_t* lab);

/* The console word in capitals, the prefix of the lab's replies and its
 * scenes' ("MY"). */
const char* lab_reply_tag(const lab_t* lab);

/* "scenes" lists each scene's key and name and which one shows; "scene <key>"
 * switches to that exact key at the next frame. "pose <ms>" holds the showing
 * scene's path there (lab_seek_path(), then paused), so a view can be
 * compared at one pose; "pose play" lets it move again. Replies are
 * "<TAG> ..." lines then "<TAG>_END", or "<TAG>_ERR <why>". Other words go to
 * hooks.console, then to the showing scene's console. */
bool lab_console(lab_t* lab, const char* args);

/* The showing scene's path clip, as its `path` hook names it: its length in
 * ms, 0 when the scene has none, has not loaded or lacks that clip. */
uint32_t lab_path_duration_ms(const lab_t* lab);

/* Places the showing scene's path at `t_ms`, wrapped to its length, cutting to
 * its clip first; a frame at dt 0 then draws that pose. False when the scene
 * has no path. */
bool lab_seek_path(const lab_t* lab, uint32_t t_ms);
