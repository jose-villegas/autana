# lab

The shell a lab app hosts its scenes behind. A lab app shows one scene at a
time: a BOOT-opened menu picks the scene, a HUD shows its title, fps and status
line, and the serial console lists and switches scenes. The app owns one
`lab_t`, registers it once with `LAB_REGISTER()`, lists its scenes and adds its
own steps as hooks; the lab does the rest. A lab needs only its name, its
console word and its scenes:

```c fragment
lab_t my_lab = {.name = "my_lab", .console_word = "my", .scenes = scenes, .scene_count = 2};
LAB_REGISTER(my_lab);
```

| File | Owns |
|---|---|
| `lab.h` | `lab_t`: entering, switching and leaving scenes, the app arena from a scene's entry on, the menu and HUD, the `scenes` / `scene <key>` / `pose <ms>` console words |
| `lab_view.h` | the view tunables every lab has, `<name>.scale`, `.present`, `.layout` and `.orbit_ms`, declared once and registered per lab |
| `lab_scene.h` | `lab_scene_t`, one scene: its hooks (enter, frame, exit, steer, update, status, controls, console, path) and loading helpers |
| `lab_orbit.h` | a camera circling one subject, turned by one finger, fitted to the subject's sphere |
| `lab_clips.h` | a scene's animation clip picker and blend slider, in the menu and the console |
| `ui/` | the HUD and menu screens |
| `test/` | device suites' fixtures (`lab_test.h`: enter a lab on a scene, draw presented frames), the perf quarters table, and the lab's own suites |

## Hooks

A hook is optional; the lab calls each at one point of its loop.

| Hook | Called |
|---|---|
| `scene_settle` | after a scene's `enter()`, and after each `frame()` it draws |
| `scene_leaving` | before a scene's `exit()` |
| `frame_begin` | before a scene's `frame()` |
| `update_end` | after the scene's `steer()` and `update()`, menu open or not |
| `console` | for console words the lab's own did not take, before the scene's |
| `test_render` | in suites only, just before a test frame renders |
| `settings` | the lab's own menu rows, under the scene's |

## Where it sits

`lab/` is the system band's row above the app contract
([Layers](../../../docs/firmware/Layers.md)): it builds on `app/`, the engine and
the platform, and an app under `apps/` may include it.
