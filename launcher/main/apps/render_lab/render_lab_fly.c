#include "render_lab_fly.h"

#include <stdio.h>
#include <string.h>

#include "math/scalar/units.h"
#include "render/context/render_context.h"
#include "scene/scene.h"

/* The one flight there can be: the camera it flies, where it began, and the
 * steering the lines since the last frame left. */
typedef struct {
    bool flying;
    scene_t* scene;
    scene_entity_t camera;
    transformf_t home;
    fly_motion_t fly;
    fly_motion_steer_t steer;
    uint32_t idle_ms;
} flight_t;

static flight_t flight;

/* Whether the camera the flight began on is still the one drawn. */
static bool
still_drawn(void) {
    scene_t* scene;
    scene_entity_t camera;
    return scene_active_camera(&scene, &camera) && scene == flight.scene && camera == flight.camera;
}

static bool
take_off(void) {
    scene_t* scene;
    scene_entity_t camera;
    if (!scene_active_camera(&scene, &camera)) {
        return false;
    }
    /* A camera with no clip has nothing to pause. */
    (void)scene_animator_set_paused(scene, camera, SCENE_ANIMATOR_CURRENT, true);
    const transformf_t home = scene_entity_local(scene, camera);
    flight = (flight_t){.flying = true,
                        .scene = scene,
                        .camera = camera,
                        .home = home,
                        .fly = fly_motion_start(home, render_lab_fly_start_speed(scene_bounds(scene)))};
    return true;
}

void
render_lab_fly_stop(void) {
    if (flight.flying && still_drawn()) {
        scene_entity_set_local(flight.scene, flight.camera, flight.home);
        (void)scene_animator_set_paused(flight.scene, flight.camera, SCENE_ANIMATOR_CURRENT, false);
        render_context_forget(render_context_main());
    }
    flight.flying = false;
}

static void
reply_pose(void) {
    if (!flight.flying) {
        (void)printf("RENDER fly=none\nRENDER_END\n");
        return;
    }
    const transformf_t* pose = &flight.fly.pose;
    (void)printf("RENDER fly position=%.3f,%.3f,%.3f rotation=%.5f,%.5f,%.5f,%.5f speed=%.3f\nRENDER_END\n",
                 (double)pose->position.x, (double)pose->position.y, (double)pose->position.z, (double)pose->rotation.x,
                 (double)pose->rotation.y, (double)pose->rotation.z, (double)pose->rotation.w,
                 (double)flight.fly.speed);
}

bool
render_lab_fly_console(const char* args) {
    if (!console_word(&args, "fly")) {
        return false;
    }
    fly_motion_steer_t steer;
    if (strcmp(args, "release") == 0) {
        render_lab_fly_stop();
        (void)printf("RENDER fly=release\nRENDER_END\n");
    } else if (strcmp(args, "pose") == 0) {
        reply_pose();
    } else if (!render_lab_fly_parse(args, &steer)) {
        (void)printf(
            "RENDER_ERR usage: render fly <right> <up> <forward> <drag x> <drag y> <wheel> | pose | release\n");
    } else if (!flight.flying && !take_off()) {
        (void)printf("RENDER_ERR no camera is drawn\n");
    } else {
        /* Silent, like TOUCH and IMU: a reply per line would flood the console it steers through. */
        flight.steer.move = steer.move;
        flight.steer.look = vec2f_add(flight.steer.look, steer.look);
        flight.steer.wheel += steer.wheel;
        flight.idle_ms = 0;
        return true;
    }
    (void)fflush(stdout);
    return true;
}

void
render_lab_fly_frame(uint32_t dt_ms) {
    if (!flight.flying) {
        return;
    }
    flight.idle_ms += dt_ms;
    if (flight.idle_ms >= RENDER_LAB_FLY_IDLE_MS) {
        render_lab_fly_stop();
        return;
    }
    if (!still_drawn()) {
        flight.flying = false;
        return;
    }
    (void)fly_motion_update(&flight.fly, &flight.steer, (float)dt_ms / (float)MATH_MS_PER_S);
    flight.steer.look = vec2f_zero();
    flight.steer.wheel = 0.0F;
    scene_entity_set_local(flight.scene, flight.camera, flight.fly.pose);
}
