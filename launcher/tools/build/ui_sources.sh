#!/bin/sh
# Shared UI translation units, relative to launcher/, for host builds.
UI_SOURCES="
main/ui/ui.c
main/ui/input/ui_bridge.c
main/ui/ui_build.c
main/ui/face/ui_face.c
main/ui/ui_canvas_marks.c
main/ui/controls/ui_icons.c
main/ui/input/ui_pointer.c
main/ui/input/ui_snap.c
main/ui/controls/ui_widgets.c
components/microui/src/microui.c
"
# The UI sources for a host renderer: the same, plus a stand-in for the haptic pulse, which a host has no motor for.
UI_RENDER_HOST_SOURCES="$UI_SOURCES
tools/render/render_host_haptics.c
"
