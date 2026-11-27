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
# A host renderer plays no sound: the UI with its stand-in for the button pulse.
UI_RENDER_HOST_SOURCES="$UI_SOURCES
tools/render/render_host_haptics.c
"
