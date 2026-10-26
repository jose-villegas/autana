# Startup

`app_main()` in `main.c` runs a fixed order, then hands over to the loop, and
the order is most of the point:

| Step | Call | Does |
|---|---|---|
| 1 | `ui_ridge_load()` | the curve from the engine pack, mounted for good; none: a level ridge |
| 2 | `shell_init()` | the shell's own state, which the self-tests step before the loop runs |
| 3 | `post_run_before_display()` | the SD card, on its own SDMMC bus |
| 4 | `display_start()` | panel up, framebuffer allocated; on failure `main.c` logs and sleeps in a loop, never returns |
| 5 | `display_load_panel_clock()` | the saved panel clock, applied |
| 6 | `post_run_after_display()` | the rest of the health check; a failure holds the screen for 8 s |
| 7 | `selftest_run()` | SELFTEST builds with autorun only |
| 8 | `shell_check_console_prefixes()` | development builds stop here on two console prefixes that clash |
| 9 | `display_reset_quarter()`, `ui_launcher_init()`, `ui_set_transform()` | the launcher exists, turned the way boot draws |
| 10 | `boot_anim_run()` | the startup animation, 5.5 s; it mounts its clip's pack; with no pack it holds a rest pose |
| 11 | `gfx_request_full_redraw()` | marks everything dirty |
| 12 | `input_start()` | touch, buttons, then the motion sensor; no sensor: the display stays upright |
| 13 | `console_start()` | development builds only |
| 14 | `shell_run()` | the frame loop, which never returns |

The health checks come first, so a faulty board says so before it does anything
decorative. Touch starts after the animation, because until then there is
nothing for a tap to reach.

The launcher is built before the animation because the animation ends *in* it.

- For its last 700 ms each frame starts from the home screen, painted by the shell
  (`shell_paint_home_under_boot()`) through `boot_anim_set_ending_backdrop()`, and
  the photograph dithers away over it.
- `boot/` takes a painter rather than calling the launcher, because it knows
  nothing above itself.
- What the animation draws: `boot/boot_anim.h`.
