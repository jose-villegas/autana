/* Fixed-pose timing formats shared by the capture parser and the suite. */
#pragma once

#define SPONZA_PERF_POSE_FORMAT "%s t=%6ums clusters=%4d tris=%5d | both cores: frame %7lldus"
#define SPONZA_PERF_SUMMARY     "%s both cores: mean %lldus (%.1f fps before present), worst %lldus, med %lldus, p95 %lldus"
