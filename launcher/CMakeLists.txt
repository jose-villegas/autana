cmake_minimum_required(VERSION 3.16)
# launcher/packages/ holds first-party libraries shipped as components of
# their own; components/ holds vendored code.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/packages")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(launcher)
