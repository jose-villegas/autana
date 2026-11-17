// A C interface over meshoptimizer's demo/clusterlod.h, for lod/levels.py:
// builds the cluster level hierarchy of one welded, position-only mesh and
// hands it back as flat arrays. levels.py builds it with meshoptimizer's sources.
#include "meshoptimizer.h"

#include <algorithm>
#include <vector>

namespace {

// The finest clusters a caller hands lod_build: triangles per cluster, each
// cluster's indices consecutive in the mesh's index buffer. clusterlod's first
// meshlet build, the split of the whole mesh, takes these instead of cutting
// its own; every later build (a group's simplified triangles) is meshoptimizer's.
struct Given {
    const unsigned int* triangles = nullptr;
    size_t count = 0;
    bool taken = false;
    bool failed = false;
};

thread_local Given given;

size_t
given_or_built(meshopt_Meshlet* meshlets, unsigned int* meshlet_vertices, unsigned char* meshlet_triangles,
               const unsigned int* indices, size_t index_count, const float* vertex_positions, size_t vertex_count,
               size_t vertex_positions_stride, size_t max_vertices, size_t min_triangles, size_t max_triangles,
               float cone_weight, float split_factor) {
    if (given.triangles == nullptr || given.taken) {
        return meshopt_buildMeshletsFlex(meshlets, meshlet_vertices, meshlet_triangles, indices, index_count,
                                         vertex_positions, vertex_count, vertex_positions_stride, max_vertices,
                                         min_triangles, max_triangles, cone_weight, split_factor);
    }
    given.taken = true;
    // clusterlod sized the buffers for meshoptimizer's own worst case.
    if (given.count > meshopt_buildMeshletsBound(index_count, max_vertices, min_triangles)) {
        given.failed = true;
        return 0;
    }
    std::vector<int> local(vertex_count, -1);
    size_t at = 0, vertex_at = 0;
    for (size_t k = 0; k < given.count; k++) {
        const size_t count = given.triangles[k];
        if (count == 0 || count > max_triangles || at + 3 * count > index_count) {
            given.failed = true;
            return 0;
        }
        meshopt_Meshlet& m = meshlets[k];
        m.vertex_offset = static_cast<unsigned int>(vertex_at);
        m.triangle_offset = static_cast<unsigned int>(at);
        m.vertex_count = 0;
        m.triangle_count = static_cast<unsigned int>(count);
        for (size_t j = at; j < at + 3 * count; j++) {
            const unsigned int v = indices[j];
            if (local[v] < 0) {
                if (m.vertex_count == max_vertices) {
                    given.failed = true;
                    return 0;
                }
                local[v] = static_cast<int>(m.vertex_count++);
                meshlet_vertices[vertex_at++] = v;
            }
            meshlet_triangles[j] = static_cast<unsigned char>(local[v]);
        }
        for (size_t j = m.vertex_offset; j < vertex_at; j++) {
            local[meshlet_vertices[j]] = -1;
        }
        at += 3 * count;
    }
    if (at != index_count) {
        given.failed = true;
        return 0;
    }
    return given.count;
}

} // namespace

#define meshopt_buildMeshletsFlex given_or_built
#define CLUSTERLOD_IMPLEMENTATION
#include "clusterlod.h"
#undef meshopt_buildMeshletsFlex

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
// locks, one per vertex or null, holds meshopt_SimplifyVertex_Lock where a
// vertex must stay put at every level.
// cluster_triangles, cluster_count: the finest clusters, or null to let
// clusterlod cut them; a null result when they do not fit its limits.
LOD_API void*
lod_build(const float* positions, size_t vertex_count, const unsigned int* indices, size_t index_count,
          size_t max_triangles, const unsigned char* locks, const unsigned int* cluster_triangles,
          size_t cluster_count) {
    clodConfig config = clodDefaultConfig(max_triangles);
    config.simplify_permissive = false;
    config.simplify_fallback_sloppy = true;
    clodMesh mesh = {};
    mesh.indices = indices;
    mesh.index_count = index_count;
    mesh.vertex_count = vertex_count;
    mesh.vertex_positions = positions;
    mesh.vertex_positions_stride = sizeof(float) * 3;
    mesh.vertex_lock = locks;
    given = Given{cluster_triangles, cluster_count};
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
    const bool failed = given.failed;
    given = Given{};
    if (failed) {
        delete out;
        return nullptr;
    }
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
