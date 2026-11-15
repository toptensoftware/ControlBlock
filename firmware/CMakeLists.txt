cmake_minimum_required(VERSION 3.22)

# Firmware version from the root package.json (see version.cmake): sets PROJECT_VER, which project() uses.
include(${CMAKE_CURRENT_LIST_DIR}/version.cmake)
cb_set_project_version()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(controlblock)
