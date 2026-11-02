// A C interface over meshoptimizer's demo/clusterlod.h, for lod/levels.py:
// builds the cluster level hierarchy of one welded, position-only mesh and
// hands it back as flat arrays. levels.py builds it with meshoptimizer's sources.
#include "meshoptimizer.h"

#define CLUSTERLOD_IMPLEMENTATION
#include "clusterlod.h"

#include <algorithm>
#include <vector>

#ifdef _WIN32
#define LOD_API extern "C" __declspec(dllexport)
#else
#define LOD_API extern "C"
#endif

namespace {

struct Levels {
    std::vector<unsigned int> indices;
    std::vector<unsigned int> cluster_first; // first index of each cluster in indices
    std::vector<float> cluster_sphere;       // 4 per cluster
    std::vector<int> cluster_refined;        // group that produced it, or -1 on the finest level
    std::vector<int> cluster_group;          // group it belongs to
    std::vector<float> group_sphere;         // 4 per group
    std::vector<float> group_error;          // FLT_MAX on a terminal group
};

} // namespace

// Regular (border-locked) simplify with the sloppy fallback for stuck groups,
// position error only. max_triangles is clusterlod's cluster size, 4..256.
LOD_API void*
lod_build(const float* positions, size_t vertex_count, const unsigned int* indices, size_t index_count,
          size_t max_triangles) {
    clodConfig config = clodDefaultConfig(max_triangles);
    config.simplify_permissive = false;
    config.simplify_fallback_sloppy = true;
    clodMesh mesh = {};
    mesh.indices = indices;
    mesh.index_count = index_count;
    mesh.vertex_count = vertex_count;
    mesh.vertex_positions = positions;
    mesh.vertex_positions_stride = sizeof(float) * 3;
    Levels* out = new Levels();
    clodBuild(config, mesh, [out](clodGroup group, const clodCluster* clusters, size_t count) -> int {
        const int id = static_cast<int>(out->group_error.size());
        out->group_sphere.insert(out->group_sphere.end(), group.simplified.center, group.simplified.center + 3);
        out->group_sphere.push_back(group.simplified.radius);
        out->group_error.push_back(group.simplified.error);
        for (size_t i = 0; i < count; i++) {
            const clodCluster& c = clusters[i];
            out->cluster_first.push_back(static_cast<unsigned int>(out->indices.size()));
            out->indices.insert(out->indices.end(), c.indices, c.indices + c.index_count);
            out->cluster_sphere.insert(out->cluster_sphere.end(), c.bounds.center, c.bounds.center + 3);
            out->cluster_sphere.push_back(c.bounds.radius);
            out->cluster_refined.push_back(c.refined);
            out->cluster_group.push_back(id);
        }
        return id;
    });
    return out;
}

LOD_API void
lod_sizes(const void* handle, size_t* clusters, size_t* indices, size_t* groups) {
    const Levels* levels = static_cast<const Levels*>(handle);
    *clusters = levels->cluster_refined.size();
    *indices = levels->indices.size();
    *groups = levels->group_error.size();
}

template <typename T>
static void
copy_out(const std::vector<T>& from, T* to) {
    std::copy(from.begin(), from.end(), to);
}

LOD_API void
lod_read(const void* handle, unsigned int* indices, unsigned int* cluster_first, float* cluster_sphere,
         int* cluster_refined, int* cluster_group, float* group_sphere, float* group_error) {
    const Levels* levels = static_cast<const Levels*>(handle);
    copy_out(levels->indices, indices);
    copy_out(levels->cluster_first, cluster_first);
    copy_out(levels->cluster_sphere, cluster_sphere);
    copy_out(levels->cluster_refined, cluster_refined);
    copy_out(levels->cluster_group, cluster_group);
    copy_out(levels->group_sphere, group_sphere);
    copy_out(levels->group_error, group_error);
}

LOD_API void
lod_free(void* handle) {
    delete static_cast<Levels*>(handle);
}
