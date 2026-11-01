/* capybara_content: scene identity, the gallop clip, its playback command and the named views. */
#pragma once

#define CAPYBARA_SCENE         "capybara"
#define CAPYBARA_SUBJECT       "capybara"
#define CAPYBARA_GALLOP        "gallop"
#define CAPYBARA_CLIP_COMMAND  "clip " CAPYBARA_GALLOP " %d"
#define CAPYBARA_CLIP_CUT_MS   0
#define CAPYBARA_CLIP_BLEND_MS 300

/* "view <name>" puts the orbit on one of these, at the home pitch. */
#define CAPYBARA_VIEW_COMMAND  "view %s"
#define CAPYBARA_VIEW_HOME     "home"
#define CAPYBARA_VIEW_FRONT    "front"
#define CAPYBARA_VIEW_SIDE     "side"
#define CAPYBARA_VIEW_BACK     "back"
#define CAPYBARA_VIEW_CLOSE    "close" /* home yaw at the zoom-in limit */
