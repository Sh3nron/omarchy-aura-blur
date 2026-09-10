cmake_minimum_required(VERSION 3.20)
project(gradual_blur_plugin LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPRLAND REQUIRED hyprland)
pkg_check_modules(JSONC REQUIRED json-c)

add_library(gradual-blur-plugin SHARED gradual_blur.cpp)
target_compile_features(gradual-blur-plugin PRIVATE cxx_std_23)
target_include_directories(gradual-blur-plugin PRIVATE ${HYPRLAND_INCLUDE_DIRS} ${JSONC_INCLUDE_DIRS})

# Mirrors upstream hyprbars: compile with cflags only. Plugin symbols are
# resolved by the running compositor; linking aquamarine/hyprutils etc. here
# makes the .so depend on their sonames, which break whenever those packages
# update (e.g. libaquamarine.so.13 -> .so.14), and Hyprland aborts inside
# dlsym when loading.
target_compile_options(gradual-blur-plugin PRIVATE -Wall -Wextra -Wpedantic)
# json-c is a stable external library the plugin parses its own config with.
# It must be LINKED explicitly: Hyprland does not expose json-c symbols for
# cross-plugin lookup, and an unresolved lazy symbol kills the whole
# compositor ("Hyprland: symbol lookup error ... json_tokener_parse").
# Everything from Hyprland's own tree (aquamarine, hyprutils, ...) stays
# unlinked and is resolved from the running compositor instead.
pkg_check_modules(JSONC_LIB REQUIRED json-c)
target_link_directories(gradual-blur-plugin PRIVATE ${JSONC_LIB_LIBRARY_DIRS})
target_link_libraries(gradual-blur-plugin PRIVATE rt ${JSONC_LIB_LIBRARIES})

# GNU unique binding is chosen while compiling, not while linking existing
# objects; it prevents safe symbol isolation between compositor and plugin.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(gradual-blur-plugin PRIVATE -fno-gnu-unique)
endif()
set_target_properties(gradual-blur-plugin PROPERTIES PREFIX "" OUTPUT_NAME "gradual-blur-plugin")
