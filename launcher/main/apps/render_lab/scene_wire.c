/*
 * scene_wire - four wireframe primitives (plane, cube, sphere, capsule) as
 * render_lab scenes, one implementation parameterised by the mesh. Vertex
 * count is the whole point: this is the rig later experiments read a
 * fps-against-vertex-count number from, so the pipeline itself
 * (wire_pipeline.h) carries no scene state of its own - only this file does,
 * sized to whichever mesh is current and freed on exit().
 */

#include <stdint.h>
#include <stdio.h>

#include "esp_heap_caps.h"

#include "../../display/display.h"
#include "../../gfx/gfx.h"
#include "render_lab.h"
#include "render_lab_scene.h"
#include "wire_pipeline.h"
#include "wire_primitives_generated.h"

#define WIRE_LINE_RGB        0x4FD1FF

#define WIRE_ORBIT_PERIOD_MS 12000

/* Elevation of the camera above the mesh's own horizon, as sin/cos in
 * S3L_F units and as a small3dlib angle (S3L_F is one turn): 30 degrees. */
#define WIRE_ELEVATION_SIN   (S3L_F / 2)
#define WIRE_ELEVATION_COS   443
#define WIRE_ELEVATION_ANGLE (S3L_F / 12)
#define WIRE_FOCAL_LENGTH    (S3L_F)

static const wire_mesh_t* current_mesh;
static wire_cs_vertex_t* cs_vertices;
static wire_segment_t* segments;
static wire_frame_t frame;
static bool alloc_ok;
static bool need_failure_clear; /* one full-screen dirty mark to erase a prior scene after a failed alloc */

static uint32_t elapsed_ms;
static r3d_view_t current_view;
static S3L_Unit current_orbit_distance;

static render_lab_coverage_t last_coverage;

/* `orbit_distance` is chosen per mesh (the one-line wrappers below) so a
 * primitive four times another's size still fills most of the screen -
 * S3L_F itself has no notion of a mesh's physical extent to derive this
 * from automatically. */
static void
wire_enter(const wire_mesh_t* mesh, S3L_Unit orbit_distance) {
    current_mesh = mesh;
    current_orbit_distance = orbit_distance;
    elapsed_ms = 0;
    last_coverage.valid = false;

    cs_vertices = heap_caps_malloc(sizeof(*cs_vertices) * mesh->vertex_count, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    segments = heap_caps_malloc(sizeof(*segments) * mesh->edge_count, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    alloc_ok = (cs_vertices != NULL && segments != NULL);
    need_failure_clear = !alloc_ok;

    if (!alloc_ok) {
        heap_caps_free(cs_vertices);
        heap_caps_free(segments);
        cs_vertices = NULL;
        segments = NULL;
        return;
    }

    frame.cs_vertices = cs_vertices;
    frame.cs_capacity = mesh->vertex_count;
    frame.segments = segments;
    frame.segment_capacity = mesh->edge_count;
    frame.segment_count = 0;
}

static void
wire_exit(void) {
    heap_caps_free(cs_vertices);
    heap_caps_free(segments);
    cs_vertices = NULL;
    segments = NULL;
    current_mesh = NULL;
}

/* Each distance is set against that primitive's bounding radius as it
 * spins, so it fills the panel's narrow axis without reaching the near
 * plane. */
static void
scene_wire_plane_enter(void) {
    wire_enter(&wire_plane_mesh, 8 * S3L_F);
}

static void
scene_wire_cube_enter(void) {
    wire_enter(&wire_cube_mesh, 7 * S3L_F / 2);
}

static void
scene_wire_sphere_enter(void) {
    wire_enter(&wire_sphere_mesh, 4 * S3L_F);
}

static void
scene_wire_capsule_enter(void) {
    wire_enter(&wire_capsule_mesh, 7 * S3L_F);
}

/* The mesh spins about its own vertical axis under a fixed camera, so the
 * camera's pitch and roll never compose with the orbit angle. A scene draws
 * in the panel's native frame, so the roll is what keeps the mesh's up on
 * the shell's current up. */
void
wire_advance_pose(uint32_t dt_ms) {
    elapsed_ms += dt_ms;

    S3L_Transform3D world;
    S3L_transform3DInit(&world);
    world.rotation.y = (S3L_Unit)(((uint64_t)elapsed_ms * S3L_F / WIRE_ORBIT_PERIOD_MS) % S3L_F);

    S3L_Transform3D camera;
    S3L_transform3DInit(&camera);
    camera.translation.y = (current_orbit_distance * WIRE_ELEVATION_SIN) / S3L_F;
    camera.translation.z = -(current_orbit_distance * WIRE_ELEVATION_COS) / S3L_F;
    camera.rotation.x = -WIRE_ELEVATION_ANGLE;
    camera.rotation.z = -display_shell_quarter() * (S3L_F / 4);

    S3L_Mat4 world_mat, camera_mat;
    S3L_makeWorldMatrix(world, world_mat);
    S3L_makeCameraMatrix(camera, camera_mat);
    S3L_mat4Xmat4(world_mat, camera_mat);

    S3L_mat4Copy(world_mat, current_view.matrix);
    current_view.focal = WIRE_FOCAL_LENGTH;
    current_view.near_z = R3D_NEAR_Z;
    current_view.center_x = GFX_WIDTH / 2;
    current_view.center_y = GFX_HEIGHT / 2;
    current_view.scale = GFX_WIDTH / 2;
}

/* Exposed for suite_wire_perf.c to time separately - the vertex stage. */
void
wire_do_transform(void) {
    if (!alloc_ok) {
        return;
    }
    wire_transform(current_mesh, &current_view, &frame);
}

/* Exposed for suite_wire_perf.c to time separately - the per-edge clip
 * stage. Leaves frame.segment_count at 0 with no work done when allocation
 * failed, so a caller never has to check alloc_ok itself. */
bool
wire_do_project(void) {
    if (!alloc_ok) {
        frame.segment_count = 0;
        return true;
    }
    return wire_project_edges(current_mesh, &current_view, GFX_WIDTH, GFX_HEIGHT, &frame);
}

/* Band mode's own commit step: marks the union of last frame's coverage and
 * this frame's, the same reason cube_transform_and_bin() marks both - a band
 * the mesh left still needs erasing even where nothing overlaps now. Runs a
 * one-time full-screen mark instead when allocation failed, so whatever the
 * previous scene left on screen still gets erased once. */
void
wire_mark_bbox_dirty(void) {
    if (!alloc_ok) {
        if (need_failure_clear) {
            gfx_mark_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT);
            need_failure_clear = false;
        }
        last_coverage.valid = false;
        return;
    }

    render_lab_coverage_mark(&last_coverage, frame.segment_count > 0, frame.bbox_x0, frame.bbox_y0, frame.bbox_x1,
                             frame.bbox_y1);
}

static void
wire_clear_frame(void) {
    /* Same technique as cube_clear_frame(): gfx_set_partial_clear() delegates
     * bounding-box erase and dirty marking of previous-frame bounds to
     * gfx_clear() itself. */
    gfx_set_partial_clear(render_lab_partial_updates);
    gfx_clear(gfx_rgb(RENDER_LAB_BACKGROUND_RGB));
}

/* Exposed for suite_wire_perf.c to time separately - full-framebuffer clear
 * plus every segment gfx_line() draws. gfx_line() clips and marks dirty
 * itself (gfx.c), so nothing here needs its own gfx_mark_dirty() call. */
void
wire_draw_full(void) {
    wire_clear_frame();
    if (!alloc_ok) {
        return;
    }

    const gfx_color_t color = gfx_rgb(WIRE_LINE_RGB);
    for (uint16_t i = 0; i < frame.segment_count; i++) {
        const wire_segment_t* s = &frame.segments[i];
        gfx_line(s->x0, s->y0, s->x1, s->y1, color);
    }
}

/* Exposed for suite_wire_perf.c to time separately - draws only the
 * segments overlapping [row0, row1). gfx_line() writes through gfx's current
 * target, which is `buf` while a band is open (gfx.c's current_target()), so
 * nothing here touches `buf` directly. */
void
wire_draw_band(gfx_color_t* buf, int row0, int row1) {
    (void)buf;
    if (!alloc_ok) {
        return;
    }

    const gfx_color_t color = gfx_rgb(WIRE_LINE_RGB);
    for (uint16_t i = 0; i < frame.segment_count; i++) {
        const wire_segment_t* s = &frame.segments[i];
        if (wire_segment_overlaps_rows(s, row0, row1)) {
            gfx_line(s->x0, s->y0, s->x1, s->y1, color);
        }
    }
}

int
wire_vertex_count(void) {
    return current_mesh != NULL ? current_mesh->vertex_count : 0;
}

int
wire_edge_count(void) {
    return current_mesh != NULL ? current_mesh->edge_count : 0;
}

int
wire_segment_count(void) {
    return frame.segment_count;
}

/* A cheap host-side-equivalent pixel estimate for the whole frame's drawn
 * segments: max(|dx|,|dy|)+1 per segment, the same count a Bresenham walk
 * would take (gfx.c's own walk()) without re-running it. */
int64_t
wire_line_pixel_estimate(void) {
    int64_t total = 0;
    for (uint16_t i = 0; i < frame.segment_count; i++) {
        const wire_segment_t* s = &frame.segments[i];
        const int dx = s->x0 > s->x1 ? s->x0 - s->x1 : s->x1 - s->x0;
        const int dy = s->y0 > s->y1 ? s->y0 - s->y1 : s->y1 - s->y0;
        total += (dx > dy ? dx : dy) + 1;
    }
    return total;
}

static const char*
wire_status(void) {
    static char buf[24];

    if (!alloc_ok) {
        return "alloc failed";
    }
    snprintf(buf, sizeof buf, "%uv %ue", (unsigned)current_mesh->vertex_count, (unsigned)current_mesh->edge_count);
    return buf;
}

static void
scene_wire_frame(uint32_t dt_ms, bool band_mode_active) {
    wire_advance_pose(dt_ms);
    wire_do_transform();
    wire_do_project();

    if (band_mode_active) {
        wire_mark_bbox_dirty();
    } else {
        wire_draw_full();
    }
}

static void
scene_wire_frame_band(gfx_color_t* buf, int row0, int row1) {
    wire_draw_band(buf, row0, row1);
}

static void
scene_wire_invalidate(void) {
    last_coverage.valid = false;
}

const render_lab_scene_t scene_wire_plane = {
    .name = "Wire Plane",
    .enter = scene_wire_plane_enter,
    .frame = scene_wire_frame,
    .frame_band = scene_wire_frame_band,
    .exit = wire_exit,
    .invalidate = scene_wire_invalidate,
    .status = wire_status,
};

const render_lab_scene_t scene_wire_cube = {
    .name = "Wire Cube",
    .enter = scene_wire_cube_enter,
    .frame = scene_wire_frame,
    .frame_band = scene_wire_frame_band,
    .exit = wire_exit,
    .invalidate = scene_wire_invalidate,
    .status = wire_status,
};

const render_lab_scene_t scene_wire_sphere = {
    .name = "Wire Sphere",
    .enter = scene_wire_sphere_enter,
    .frame = scene_wire_frame,
    .frame_band = scene_wire_frame_band,
    .exit = wire_exit,
    .invalidate = scene_wire_invalidate,
    .status = wire_status,
};

const render_lab_scene_t scene_wire_capsule = {
    .name = "Wire Capsule",
    .enter = scene_wire_capsule_enter,
    .frame = scene_wire_frame,
    .frame_band = scene_wire_frame_band,
    .exit = wire_exit,
    .invalidate = scene_wire_invalidate,
    .status = wire_status,
};
