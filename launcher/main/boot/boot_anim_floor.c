/* boot_anim_floor: fixed-tick spring replay and boot-scoped floor storage. */
#include "boot/boot_anim.h"

#include "core/memory.h"

void
boot_anim_floor_close(boot_anim_floor_t* floor) {
    memory_free(floor->grid.offset);
    memory_free(floor->grid.velocity);
    *floor = (boot_anim_floor_t){0};
}

void
boot_anim_floor_open(boot_anim_floor_t* floor) {
    *floor = (boot_anim_floor_t){0};
    int32_t* offset = memory_alloc(BOOT_ANIM_FLOOR_NODES * sizeof(*offset), MEMORY_PSRAM);
    int32_t* velocity = memory_alloc(BOOT_ANIM_FLOOR_NODES * sizeof(*velocity), MEMORY_PSRAM);
    if (offset == NULL || velocity == NULL) {
        memory_free(offset);
        memory_free(velocity);
        return;
    }
    spring_grid_init(&floor->grid, offset, velocity, BOOT_ANIM_FLOOR_SIDE, BOOT_ANIM_FLOOR_SIDE,
                     BOOT_ANIM_FLOOR_TENSION, BOOT_ANIM_FLOOR_STIFFNESS, BOOT_ANIM_FLOOR_DAMPING);
}

void
boot_anim_floor_advance(boot_anim_floor_t* floor, uint32_t now_ms) {
    if (floor->grid.offset == NULL) {
        return;
    }
    const uint32_t target = now_ms / BOOT_ANIM_FLOOR_TICK_MS;
    while (floor->ticks < target) {
        const uint32_t tick_ms = (floor->ticks + 1) * BOOT_ANIM_FLOOR_TICK_MS;
        const int32_t reached = boot_anim_pen_t(tick_ms);
        while (floor->next_zero < BOOT_ANIM_ZEROS && boot_anim_zero_t[floor->next_zero] <= reached) {
            spring_grid_poke(&floor->grid, BOOT_ANIM_MESH_CELLS / 2, BOOT_ANIM_MESH_CELLS / 2,
                             BOOT_ANIM_FLOOR_PLUCK_CELLS, BOOT_ANIM_FLOOR_PLUCK_Q16);
            floor->next_zero++;
        }
        spring_grid_tick(&floor->grid);
        floor->ticks++;
    }
}

int32_t
boot_anim_floor_height(const boot_anim_floor_t* floor, int x, int y) {
    if (floor->grid.offset == NULL) {
        return 0;
    }
    const int32_t metres = floor->grid.offset[(y * BOOT_ANIM_FLOOR_SIDE) + x];
    return (int32_t)(((int64_t)metres * (1 << BOOT_ANIM_TQ) * R3D_X_UNIT_ONE)
                     / ((int64_t)MATHX_ONE * BOOT_ANIM_SPIRAL_Q9));
}
