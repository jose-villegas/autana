#include "lab/lab.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app_arena.h"
#include "build/build_variant.h"
#include "esp_log.h"
#include "gfx/gfx.h"
#include "image/pixel/rgb565.h"
#include "lab/ui/lab_hud_screen.h"
#include "lab/ui/lab_menu_screen.h"
#include "math/scalar/units.h"
#include "scene/animator/animator.h"
#include "scene/entity/entity.h"
#include "ui/ui.h"

/* How long a scene's name stays on screen after the scene is entered. The
 * BOOT menu shows the name at any time, so the HUD does not keep it. */
#define SCENE_TITLE_MS      2000
#define SCENE_TITLE_FADE_MS 500
#define TITLE_OPAQUE        255

/* On-screen framerate, since the shell's report_fps() reaches only the
 * serial console. Windowed on the dt_ms lab_frame() is handed. */
#define FPS_WINDOW_MS       500

/* The longest scene list a menu frame holds: the static lists below. */
#define LAB_SCENES_MAX      64

/* "my pose <ms>": the words before the number. */
#define POSE_WORD           "pose "
#define SCENE_WORD          "scene "

void
lab_register(lab_t* lab) {
    size_t i = 0;
    for (; lab->console_word[i] != '\0' && i + 1 < sizeof lab->reply_tag; i++) {
        lab->reply_tag[i] = (char)toupper((unsigned char)lab->console_word[i]);
    }
    assert(lab->console_word[i] == '\0');
    lab->reply_tag[i] = '\0';
}

bool
lab_menu_open(const lab_t* lab) {
    return lab->state.menu_open;
}

const char*
lab_reply_tag(const lab_t* lab) {
    return lab->reply_tag;
}

const lab_scene_t*
lab_current(const lab_t* lab) {
    return lab->scenes[lab->state.scene];
}

/* Unknown or unset resolves to the first scene, never a hard error. */
static int
scene_index_for_key(const lab_t* lab, const char* key) {
    if (key != NULL) {
        for (int i = 0; i < lab->scene_count; i++) {
            if (strcmp(lab->scenes[i]->key, key) == 0) {
                return i;
            }
        }
    }
    return 0;
}

static uint8_t
scene_title_alpha(const lab_t* lab) {
    if (lab->state.title_ms >= SCENE_TITLE_FADE_MS) {
        return TITLE_OPAQUE;
    }
    return (uint8_t)(lab->state.title_ms * TITLE_OPAQUE / SCENE_TITLE_FADE_MS);
}

static void
enter_scene(lab_t* lab) {
    lab->state.scene_mark = app_arena_mark();
    lab_current(lab)->enter(lab_current(lab));
    if (lab->hooks.scene_settle != NULL) {
        lab->hooks.scene_settle();
    }
    lab->state.title_ms = SCENE_TITLE_MS;
}

/* Everything the scene took (its scene, the render context's scratch, its
 * clip list) goes back by rewinding the arena after its exit(). */
static void
exit_scene(lab_t* lab) {
    if (lab->hooks.scene_leaving != NULL) {
        lab->hooks.scene_leaving();
    }
    lab_current(lab)->exit();
    app_arena_rewind(lab->state.scene_mark);
}

typedef struct {
    gfx_layout_t layout;
    bool clear_on_send;
} layout_choice_t;

static const layout_choice_t layout_choices[LAB_LAYOUT_COUNT] = {
    [LAB_LAYOUT_FULL] = {GFX_LAYOUT_FULL_FB, false},
    [LAB_LAYOUT_DOUBLE] = {GFX_LAYOUT_DOUBLE_FB, false},
    [LAB_LAYOUT_DOUBLE_CLEAR] = {GFX_LAYOUT_DOUBLE_FB, true},
};

static void
enter_layout(const lab_t* lab) {
    const layout_choice_t* chosen = &layout_choices[lab_view_layout(lab->view)];
    const gfx_mode_request_t mode_request = {
        .layout = chosen->layout,
        .interlace_x = false,
        .interlace_y = false,
        .clear_on_send = chosen->clear_on_send,
        .clear_colour = RGB565BE(UI_BACKGROUND),
    };
    (void)gfx_mode_enter(&mode_request);
    gfx_request_full_redraw();
}

/* Always on the scene view, menu closed, with a fresh orientation baseline,
 * so a rotation while another app showed does not read as a change on the
 * first frame back. */
void
lab_enter(lab_t* lab) {
    assert(lab->scene_count <= LAB_SCENES_MAX);
    lab->state = (lab_state_t){.scene = scene_index_for_key(lab, lab->start_key), .requested = -1};
    enter_layout(lab);
    enter_scene(lab);
    lab->state.layout_gen = ui_layout_generation();
}

/* The new scene clears the full framebuffer, so the composed half picture
 * is discarded first. */
static void
switch_to_scene(lab_t* lab, int index) {
    exit_scene(lab);
    lab->state.scene = index;
    lab->start_key = lab_current(lab)->key;
    gfx_mode_exit();
    enter_layout(lab);
    ui_invalidate();
    enter_scene(lab);
}

/* Scene families in the order their first scene appears. */
static int
family_names(const lab_t* lab, const char* names[LAB_SCENES_MAX]) {
    int count = 0;
    for (int i = 0; i < lab->scene_count; i++) {
        assert(lab->scenes[i]->family != NULL);
        int seen = 0;
        while (seen < count && strcmp(names[seen], lab->scenes[i]->family) != 0) {
            seen++;
        }
        if (seen == count) {
            names[count++] = lab->scenes[i]->family;
        }
    }
    return count;
}

/* The family's scenes: their variant names, the index of the showing one (-1 if another family shows). */
static int
family_variants(const lab_t* lab, const char* family, const char* variants[LAB_SCENES_MAX], int* selected) {
    int count = 0;
    *selected = -1;
    for (int i = 0; i < lab->scene_count; i++) {
        if (strcmp(lab->scenes[i]->family, family) == 0) {
            if (i == lab->state.scene) {
                *selected = count;
            }
            variants[count++] = lab->scenes[i]->variant;
        }
    }
    for (int i = 0; count > 1 && i < count; i++) {
        assert(variants[i] != NULL);
    }
    return count;
}

/* Switches to the family's index-th scene at the next frame. */
static void
request_variant(lab_t* lab, const char* family, int index) {
    int count = 0;
    for (int i = 0; i < lab->scene_count; i++) {
        if (strcmp(lab->scenes[i]->family, family) == 0 && count++ == index) {
            lab->state.requested = i;
            return;
        }
    }
}

/* A release draws only the scene title, so its HUD is gone once the title has faded. */
static bool
hud_showing(const lab_t* lab) {
    return lab->show_hud && (BUILD_VARIANT_DEVELOPMENT_OR_HOST || lab->state.title_ms > 0);
}

/* The scene changes underneath every frame, so the HUD has no opaque window. */
static void
draw_hud(const lab_t* lab, const input_t* input) {
    ui_begin(input);
    const lab_scene_t* scene = lab_current(lab);
    const lab_hud_screen_state_t state = {
        .scene_title = scene->name,
        .scene_title_alpha = scene_title_alpha(lab),
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
        .fps_value = lab->state.fps,
        .status = scene->status != NULL ? scene->status() : NULL,
#endif
    };
    lab_hud_screen_draw(&state);
    ui_end(UI_NO_BACKGROUND);
}

/* Its lists are static: the menu draws on the main task alone, whose stack
 * the scene list would otherwise grow with every scene. */
static void
draw_menu(lab_t* lab, const input_t* input) {
    ui_begin(input);
    static const char* families[LAB_SCENES_MAX];
    const int family_count = family_names(lab, families);
    int family = 0;
    while (strcmp(families[family], lab_current(lab)->family) != 0) {
        family++;
    }
    static const char* variants[LAB_SCENES_MAX];
    int variant = 0;
    const int variant_count = family_variants(lab, families[family], variants, &variant);
    const lab_menu_screen_state_t state = {
        .families = families,
        .family_count = family_count,
        .family = family,
        .variants = variants,
        .variant_count = variant_count,
        .variant = variant,
        .controls = lab_current(lab)->controls,
        .settings = lab->hooks.settings,
    };
    const lab_menu_screen_result_t result = lab_menu_screen_draw(&state);
    if (result.family_picked >= 0 && result.family_picked != family) {
        request_variant(lab, families[result.family_picked], 0);
    }
    if (result.variant_picked >= 0 && result.variant_picked != variant) {
        request_variant(lab, families[family], result.variant_picked);
    }
    ui_end(UI_BACKGROUND);
}

static void
update_scene_title(lab_t* lab, uint32_t dt_ms) {
    if (lab->state.title_ms == 0) {
        return;
    }
    lab->state.title_ms = lab->state.title_ms > dt_ms ? lab->state.title_ms - dt_ms : 0;
    if (lab->state.title_ms == 0) {
        gfx_request_full_redraw();
    }
}

static void
update_fps(lab_state_t* s, uint32_t dt_ms) {
    s->fps_frames++;
    s->fps_window_ms += dt_ms;
    if (s->fps_window_ms >= FPS_WINDOW_MS) {
        s->fps = (float)s->fps_frames * (float)MATH_MS_PER_S / (float)s->fps_window_ms;
        s->fps_frames = 0;
        s->fps_window_ms = 0;
    }
}

static void
toggle_menu(lab_t* lab) {
    lab->state.menu_open = !lab->state.menu_open;
    scene_set_paused(lab->state.menu_open);
    gfx_request_full_redraw();
    if (lab->state.menu_open) {
        ui_invalidate();
    }
}

/* The fps counts the scene view's frames only, never the menu's. */
void
lab_frame(lab_t* lab, uint32_t dt_ms, const input_t* input) {
    if (lab->state.requested >= 0) {
        if (lab->state.requested != lab->state.scene) {
            switch_to_scene(lab, lab->state.requested);
        }
        lab->state.requested = -1;
    }
    if (input->boot.pressed) {
        toggle_menu(lab);
    }
    const uint32_t layout_gen = ui_layout_generation();
    if (layout_gen != lab->state.layout_gen) {
        lab->state.layout_gen = layout_gen;
        gfx_request_full_redraw();
    }
    if (lab->state.menu_open) {
        draw_menu(lab, input);
        return;
    }
    if (BUILD_VARIANT_DEVELOPMENT_OR_HOST) {
        update_fps(&lab->state, dt_ms);
    }
    const bool hud = hud_showing(lab);
    update_scene_title(lab, dt_ms);
    if (lab->hooks.frame_begin != NULL) {
        lab->hooks.frame_begin(lab_current(lab));
    }
    lab_current(lab)->frame(dt_ms);
    if (lab->hooks.scene_settle != NULL) {
        lab->hooks.scene_settle();
    }
    if (hud) {
        draw_hud(lab, input);
    }
}

void
lab_exit(lab_t* lab) {
    exit_scene(lab);
    gfx_mode_exit();
}

/* The scene's own steer() and update(), overlapped with the send of the
 * frame drawn last pass, then the app's hook, last so it wins over a scene
 * that places its own camera. A switch or the menu takes effect in
 * lab_frame(), after this, so a scene must cope with a frame() that has no
 * matching update(). */
void
lab_update(lab_t* lab, uint32_t dt_ms, const input_t* input) {
    const lab_scene_t* scene = lab_current(lab);
    if (!lab->state.menu_open) {
        if (scene->steer != NULL) {
            scene->steer(dt_ms, input);
        }
        if (scene->update != NULL) {
            scene->update(dt_ms);
        }
    }
    if (lab->hooks.update_end != NULL) {
        lab->hooks.update_end(dt_ms);
    }
}

/* The showing scene's path, with no scene when it has none or has not loaded. */
static lab_path_t
shown_path(const lab_t* lab) {
    const lab_scene_t* scene = lab_current(lab);
    return scene->path == NULL ? (lab_path_t){.scene = ENTITY_NONE, .entity = ENTITY_NONE} : scene->path();
}

/* The path's clip, -1 when the scene has none or lacks the clip it names. */
static int
path_clip(const lab_path_t* path) {
    if (handle_is_none(path->scene) || path->clip == NULL) {
        return handle_is_none(path->scene) ? -1 : animator_current_clip(path->entity);
    }
    for (int clip = 0; clip < animator_clip_count(path->entity); clip++) {
        if (strcmp(animator_clip_name(path->entity, clip), path->clip) == 0) {
            return clip;
        }
    }
    return -1;
}

uint32_t
lab_path_duration_ms(const lab_t* lab) {
    const lab_path_t path = shown_path(lab);
    if (path.seek != NULL) {
        return path.duration_ms;
    }
    const int clip = path_clip(&path);
    return clip < 0 ? 0 : animator_clip_duration_ms(path.entity, clip);
}

bool
lab_seek_path(const lab_t* lab, uint32_t t_ms) {
    const lab_path_t path = shown_path(lab);
    const uint32_t duration_ms = lab_path_duration_ms(lab);
    if (path.seek != NULL) {
        return duration_ms > 0 && path.seek(t_ms % duration_ms);
    }
    const int clip = path_clip(&path);
    if (duration_ms == 0 || (clip != animator_current_clip(path.entity) && !animator_play(path.entity, clip, 0))) {
        return false;
    }
    return animator_seek(path.entity, ANIMATOR_CURRENT, t_ms % duration_ms);
}

/* "pose <ms> | play": the reply to the words after "pose ". */
static void
pose_console(const lab_t* lab, const char* rest) {
    const char* tag = lab->reply_tag;
    const lab_path_t path = shown_path(lab);
    const bool play = strcmp(rest, "play") == 0;
    char* end;
    const unsigned long t_ms = strtoul(rest, &end, 10);
    if (lab_path_duration_ms(lab) == 0) {
        (void)printf("%s_ERR scene %s has no path\n", tag, lab_current(lab)->key);
    } else if (play) {
        if (path.seek != NULL) {
            path.play();
        } else {
            (void)animator_set_paused(path.entity, ANIMATOR_CURRENT, false);
        }
        (void)printf("%s pose=play\n%s_END\n", tag, tag);
    } else if (!isdigit((unsigned char)rest[0]) || *end != '\0' || t_ms > UINT32_MAX
               || !lab_seek_path(lab, (uint32_t)t_ms)
               || (path.seek == NULL && !animator_set_paused(path.entity, ANIMATOR_CURRENT, true))) {
        (void)printf("%s_ERR usage: %s pose <ms> | %s pose play\n", tag, lab->console_word, lab->console_word);
    } else {
        (void)printf("%s pose=%lu\n%s_END\n", tag, t_ms, tag);
    }
    (void)fflush(stdout);
}

static void
list_scenes(const lab_t* lab) {
    for (int i = 0; i < lab->scene_count; i++) {
        (void)printf("%s scene=%s name=%s current=%d\n", lab->reply_tag, lab->scenes[i]->key, lab->scenes[i]->name,
                     i == lab->state.scene);
    }
    (void)printf("%s_END\n", lab->reply_tag);
    (void)fflush(stdout);
}

static void
request_scene(lab_t* lab, const char* key) {
    for (int i = 0; i < lab->scene_count; i++) {
        if (strcmp(lab->scenes[i]->key, key) == 0) {
            lab->state.requested = i;
            (void)printf("%s scene=%s\n%s_END\n", lab->reply_tag, lab->scenes[i]->key, lab->reply_tag);
            (void)fflush(stdout);
            return;
        }
    }
    (void)printf("%s_ERR unknown scene '%s'; `%s scenes` lists them\n", lab->reply_tag, key, lab->console_word);
    (void)fflush(stdout);
}

bool
lab_console(lab_t* lab, const char* args) {
    if (strcmp(args, "scenes") == 0) {
        list_scenes(lab);
        return true;
    }
    if (strncmp(args, SCENE_WORD, sizeof SCENE_WORD - 1) == 0) {
        request_scene(lab, args + sizeof SCENE_WORD - 1);
        return true;
    }
    if (strncmp(args, POSE_WORD, sizeof POSE_WORD - 1) == 0) {
        pose_console(lab, args + sizeof POSE_WORD - 1);
        return true;
    }
    if (lab->hooks.console != NULL && lab->hooks.console(args)) {
        return true;
    }
    const lab_scene_t* scene = lab_current(lab);
    return scene->console != NULL && scene->console(args, console_reply_stdio);
}

void
lab_scene_fail(const char* tag, char* failure, size_t size, const char* what) {
    ESP_LOGE(tag, "%s; the scene stays blank", what);
    if (snprintf(failure, size, "%s", what) < 0) {
        failure[0] = '\0';
    }
}

/* Its own frame, so the reason is not on the stack while the scene loads. */
static __attribute__((noinline)) void
fail_load(const char* tag, char* failure, size_t size, entity_t root, const char* label) {
    scene_failure_t why = {.status = SCENE_ERR_FULL}; /* what scene_load() leaves when it returns no root */
    (void)scene_failure(root, &why);
    scene_release(root);
    const bool missing = why.asset == ASSET_ERR_NOT_FOUND || why.asset == ASSET_ERR_NO_PACK;
    char what[LAB_STATUS_LEN];
    if (snprintf(what, sizeof what, missing ? "no %s pack: flash it" : "%s did not load", label) < 0) {
        what[0] = '\0';
    }
    lab_scene_fail(tag, failure, size, what);
}

entity_t
lab_scene_load(const char* tag, char* failure, size_t size, const char* id, const char* label) {
    const entity_t root = scene_load(id);
    if (scene_state(root) == PENDING_READY) {
        return root;
    }
    fail_load(tag, failure, size, root, label);
    return ENTITY_NONE;
}

void*
lab_scene_take(size_t bytes) {
    return app_arena_take(bytes, _Alignof(max_align_t));
}

/* The longest copy name a lab_scene_copies() format makes, with its terminator. */
#define LAB_COPY_NAME_SIZE 16

/* The copy `scene` names `format` with `index`, or ENTITY_NONE. */
static entity_t
find_copy(entity_t scene, const char* format, int index) {
    char name[LAB_COPY_NAME_SIZE];
    const int length = snprintf(name, sizeof name, format, index);
    return length > 0 && (size_t)length < sizeof name ? entity_find(scene, name_hash(name)) : ENTITY_NONE;
}

entity_t*
lab_scene_copies(entity_t scene, const char* format, int* count) {
    assert(strchr(format, '%') != NULL); /* the copy's number */
    *count = 0;
    int found = 0;
    while (!handle_is_none(find_copy(scene, format, found))) {
        found++;
    }
    entity_t* copies = lab_scene_take(sizeof *copies * (size_t)(found > 0 ? found : 1));
    if (copies == NULL) {
        return NULL;
    }
    for (int i = 0; i < found; i++) {
        copies[i] = find_copy(scene, format, i);
    }
    *count = found;
    return copies;
}
