cmake_minimum_required(VERSION 3.21)
project(gradual_blur_observer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick Network)

add_library(gradualblurobserverplugin MODULE
  src/PopupGeometryObserver.cpp
  src/PopupGeometryObserver.h
  src/Plugin.cpp
)

target_link_libraries(gradualblurobserverplugin PRIVATE
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network
)

set_target_properties(gradualblurobserverplugin PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native"
)
