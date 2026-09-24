idf_component_register(SRCS "pmic_cold_boot.c" REQUIRES hal esp_rom log soc)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SDKCONFIG}")
file(STRINGS "${SDKCONFIG}" pmic_psram_speed REGEX "^CONFIG_SPIRAM_SPEED_120M=y$")
if(pmic_psram_speed)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PMIC_PSRAM_120M=1)
endif()
