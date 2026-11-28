// The headers a C++ consumer drives a run through compile as C++: no
// implicit conversion from void*, among the C-only rules.
// C11's spelling of the assertion C++ calls static_assert.
#define _Static_assert static_assert
extern "C" {
#include "anim/anim_track.h"
#include "asset/asset_store.h"
#include "core/tune.h"
#include "hash/name_hash64.h"
#include "render/context/render_context.h"
#include "render/view/raster_probes.h"
#include "render_host.h"
#include "scene/animator/animator.h"
#include "scene/camera/camera.h"
#include "scene/scene.h"
#include "scene/transform/transform.h"
#include "scene_viewer.h"
#include "stream/stream_provider.h"
}
#undef _Static_assert
