# collections: containers and small algorithms over caller-owned storage
# (README.md). Header-only and depending on nothing but the C library, so it
# registers with no sources and no REQUIRES and every layer may use it.
if(ESP_PLATFORM)
    idf_component_register(INCLUDE_DIRS "include")
else()
    add_library(collections INTERFACE)
    target_include_directories(collections INTERFACE include)
endif()
