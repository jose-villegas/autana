# physics: state advanced by integrating forces (README.md). Header-only and
# using math, so it registers with no sources and declares math once below for
# both builds (docs/firmware/Layers.md).
set(physics_requires math)

if(ESP_PLATFORM)
    idf_component_register(INCLUDE_DIRS "include" REQUIRES ${physics_requires})
else()
    add_library(physics INTERFACE)
    target_include_directories(physics INTERFACE include)
    target_link_libraries(physics INTERFACE ${physics_requires})
endif()
