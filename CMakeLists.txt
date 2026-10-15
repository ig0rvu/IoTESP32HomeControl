# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Keep esp_app_desc_t.version (baked into the compiled .bin at a fixed offset,
# and what the server's upload-time check in FirmwareController.cs reads back
# out) in lockstep with FIRMWARE_VERSION from main/version.h (what the device
# actually reports at runtime over the wire via RegisterController/Heartbeat).
# Without this, PROJECT_VER falls back to `git describe`, a completely
# different string that will never match a semver tag an admin types in --
# the server-side check would reject every upload.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/main/version.h" FIRMWARE_VERSION_LINE REGEX "FIRMWARE_VERSION")
string(REGEX MATCH "\"([^\"]+)\"" _ ${FIRMWARE_VERSION_LINE})
set(PROJECT_VER ${CMAKE_MATCH_1})
# file(STRINGS ...) above only re-runs when CMake reconfigures -- which an
# unrelated CMakeLists.txt change triggers automatically, but editing
# version.h alone does not, unless it's declared as a configure dependency
# here. Without this, bumping version.h and running an ordinary incremental
# `idf.py build` silently keeps embedding the previous PROJECT_VER.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/main/version.h")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(HomeAtPalm)
