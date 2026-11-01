// The headers a C++ consumer drives a run through compile as C++: no
// implicit conversion from void*, among the C-only rules.
// C11's spelling of the assertion C++ calls static_assert.
#define _Static_assert static_assert
extern "C" {
#include "asset/asset_store.h"
#include "render/context/render_context.h"
#include "render_host.h"
#include "scene_viewer.h"
}
#undef _Static_assert
