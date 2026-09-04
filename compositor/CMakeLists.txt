cmake_minimum_required(VERSION 3.20)
project(gradual_blur_plugin LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPRLAND REQUIRED hyprland)
pkg_check_modules(JSONC REQUIRED json-c)

add_library(gradual-blur-plugin SHARED gradual_blur.cpp)
target_compile_features(gradual-blur-plugin PRIVATE cxx_std_23)
target_compile_options(gradual-blur-plugin PRIVATE -Wall -Wextra -Wpedantic ${HYPRLAND_CFLAGS_OTHER})
target_include_directories(gradual-blur-plugin PRIVATE ${HYPRLAND_INCLUDE_DIRS} ${JSONC_INCLUDE_DIRS})
target_link_directories(gradual-blur-plugin PRIVATE ${HYPRLAND_LIBRARY_DIRS})
target_link_libraries(gradual-blur-plugin PRIVATE ${HYPRLAND_LIBRARIES} ${JSONC_LIBRARIES})
set_target_properties(gradual-blur-plugin PROPERTIES PREFIX "" OUTPUT_NAME "gradual-blur-plugin")
