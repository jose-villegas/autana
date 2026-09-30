// A flat C interface to meshoptimizer's clusterlod example (the submodule's
// demo/clusterlod.h), so ctypes can drive it: no struct is passed by value
// and the result is read back through indexed getters. Built into the same
// library as the simplifier by meshopt.py.
#include <math.h>
#include <string.h>

#include <vector>

#include "meshoptimizer.h"

#define CLUSTERLOD_IMPLEMENTATION
#include "clusterlod.h"

namespace {

struct Cluster {
    int group;   // the group this cluster is an input of
    int refined; // the group that produced it, or -1 for original geometry
    clodBounds bounds;
    float cone[4]; // axis xyz, cutoff
    std::vector<unsigned int> indices;
};

struct Result {
    std::vector<clodGroup> groups;
    std::vector<Cluster> clusters;
    const float* positions;
    size_t vertex_count;
};

int collect(void* context, clodGroup group, const clodCluster* clusters, size_t count) {
    Result* r = static_cast<Result*>(context);
    int id = int(r->groups.size());
    r->groups.push_back(group);
    for (size_t i = 0; i < count; ++i) {
        Cluster c;
        c.group = id;
        c.refined = clusters[i].refined;
        c.bounds = clusters[i].bounds;
        c.indices.assign(clusters[i].indices, clusters[i].indices + clusters[i].index_count);
        meshopt_Bounds b = meshopt_computeClusterBounds(
            clusters[i].indices, clusters[i].index_count, r->positions, r->vertex_count, 12);
        c.cone[0] = b.cone_axis[0];
        c.cone[1] = b.cone_axis[1];
        c.cone[2] = b.cone_axis[2];
        c.cone[3] = b.cone_cutoff;
        r->clusters.push_back(c);
    }
    return id;
}

} // namespace

extern "C" {

// positions: 3 floats per vertex; colours: 3 floats (0..1) per vertex, held
// as simplification attributes and protected where two vertices at one
// position differ; locks: one byte per vertex, non-zero pins it.
MESHOPTIMIZER_API void* r3d_lod_build(const float* positions, const float* colours, size_t vertex_count,
                                      const unsigned int* indices, size_t index_count, const unsigned char* locks,
                                      size_t max_triangles, size_t partition_size, float colour_weight) {
    Result* r = new Result();
    r->positions = positions;
    r->vertex_count = vertex_count;

    clodConfig config = clodDefaultConfig(max_triangles);
    config.partition_size = partition_size;

    clodMesh mesh = {};
    mesh.indices = indices;
    mesh.index_count = index_count;
    mesh.vertex_count = vertex_count;
    mesh.vertex_positions = positions;
    mesh.vertex_positions_stride = 12;
    mesh.vertex_attributes = colours;
    mesh.vertex_attributes_stride = 12;
    mesh.vertex_lock = locks;
    const float weights[3] = {colour_weight, colour_weight, colour_weight};
    mesh.attribute_weights = weights;
    mesh.attribute_count = 3;
    mesh.attribute_protect_mask = 7;

    clodBuild(config, mesh, r, collect);
    return r;
}

MESHOPTIMIZER_API void r3d_lod_free(void* handle) { delete static_cast<Result*>(handle); }

MESHOPTIMIZER_API size_t r3d_lod_group_count(void* handle) { return static_cast<Result*>(handle)->groups.size(); }

MESHOPTIMIZER_API size_t r3d_lod_cluster_count(void* handle) {
    return static_cast<Result*>(handle)->clusters.size();
}

// out: centre xyz, radius, error.
MESHOPTIMIZER_API void r3d_lod_group(void* handle, size_t i, int* depth, float* out) {
    const clodGroup& g = static_cast<Result*>(handle)->groups[i];
    *depth = g.depth;
    memcpy(out, g.simplified.center, 3 * sizeof(float));
    out[3] = g.simplified.radius;
    out[4] = g.simplified.error;
}

// bounds: centre xyz, radius, error; cone: axis xyz, cutoff.
MESHOPTIMIZER_API size_t r3d_lod_cluster(void* handle, size_t i, int* group, int* refined, float* bounds,
                                         float* cone) {
    const Cluster& c = static_cast<Result*>(handle)->clusters[i];
    *group = c.group;
    *refined = c.refined;
    memcpy(bounds, c.bounds.center, 3 * sizeof(float));
    bounds[3] = c.bounds.radius;
    bounds[4] = c.bounds.error;
    memcpy(cone, c.cone, 4 * sizeof(float));
    return c.indices.size();
}

MESHOPTIMIZER_API void r3d_lod_cluster_indices(void* handle, size_t i, unsigned int* out) {
    const Cluster& c = static_cast<Result*>(handle)->clusters[i];
    memcpy(out, c.indices.data(), c.indices.size() * sizeof(unsigned int));
}

} // extern "C"
