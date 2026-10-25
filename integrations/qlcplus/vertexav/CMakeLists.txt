# VertexAV Link input plugin for QLC+.
#
# Two ways to build:
#  * inside the QLC+ tree: copy this folder to qlcplus/plugins/vertexav (with libvertexav in
#    plugins/vertexav/libvertexav) and add "add_subdirectory(vertexav)" to plugins/CMakeLists.txt
#    (install-into-qlcplus.sh does both);
#  * standalone, against a QLC+ source checkout:
#      cmake -S . -B build -DQLCPLUS_SOURCE_DIR=/path/to/qlcplus
cmake_minimum_required(VERSION 3.16)

set(module_name "vertexavplugin")

if(NOT DEFINED QLC_PLUGIN_TYPE)
  project(qlcplus-vertexav LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_AUTOMOC ON)
  set(QLCPLUS_SOURCE_DIR "" CACHE PATH "QLC+ source checkout (for plugins/interfaces)")
  if(NOT QLCPLUS_SOURCE_DIR)
    message(FATAL_ERROR "Set QLCPLUS_SOURCE_DIR to a QLC+ source checkout")
  endif()
  find_package(Qt6 COMPONENTS Core QUIET)
  if(Qt6_FOUND)
    set(QT_MAJOR_VERSION 6)
  else()
    set(QT_MAJOR_VERSION 5)
  endif()
  find_package(Qt${QT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Network Widgets)
  set(QLC_PLUGIN_TYPE MODULE)
  set(VERTEXAV_INTERFACES_DIR ${QLCPLUS_SOURCE_DIR}/plugins/interfaces)
  set(VERTEXAV_STANDALONE ON)
else()
  set(VERTEXAV_INTERFACES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../interfaces)
  set(VERTEXAV_STANDALONE OFF)
endif()

# libvertexav: bundled copy first, then the one in the vertexav-link repository.
if(NOT VERTEXAV_LIB_DIR)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/libvertexav/CMakeLists.txt)
    set(VERTEXAV_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libvertexav)
  else()
    set(VERTEXAV_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../libvertexav)
  endif()
endif()
set(VERTEXAV_QT_MAJOR ${QT_MAJOR_VERSION} CACHE STRING "" FORCE)
set(VERTEXAV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VERTEXAV_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(${VERTEXAV_LIB_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libvertexav)

add_library(${module_name} ${QLC_PLUGIN_TYPE})
target_sources(${module_name} PRIVATE
  ${VERTEXAV_INTERFACES_DIR}/qlcioplugin.cpp ${VERTEXAV_INTERFACES_DIR}/qlcioplugin.h
  vertexavbridge.cpp vertexavbridge.h
  vertexavchannels.h
  vertexavconfiguration.cpp vertexavconfiguration.h
  vertexavplugin.cpp vertexavplugin.h
)
target_include_directories(${module_name} PRIVATE ${VERTEXAV_INTERFACES_DIR})
target_link_libraries(${module_name} PRIVATE
  vertexav
  Qt${QT_MAJOR_VERSION}::Core
  Qt${QT_MAJOR_VERSION}::Gui
  Qt${QT_MAJOR_VERSION}::Network
  Qt${QT_MAJOR_VERSION}::Widgets
)

if(NOT VERTEXAV_STANDALONE)
  install(TARGETS ${module_name}
    LIBRARY DESTINATION ${INSTALLROOT}/${PLUGINDIR}
    RUNTIME DESTINATION ${INSTALLROOT}/${PLUGINDIR}
  )
  if(UNIX AND NOT APPLE)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/org.qlcplus.QLCPlus.vertexav.metainfo.xml"
            DESTINATION ${METAINFODIR})
  endif()
else()
  # Test harness: loads the plugin like QLC+ does and records its input channels.
  add_executable(vertexav_plugin_harness ${CMAKE_CURRENT_SOURCE_DIR}/../test/pluginharness.cpp
    ${VERTEXAV_INTERFACES_DIR}/qlcioplugin.cpp)
  target_include_directories(vertexav_plugin_harness PRIVATE ${VERTEXAV_INTERFACES_DIR})
  target_link_libraries(vertexav_plugin_harness PRIVATE Qt${QT_MAJOR_VERSION}::Core Qt${QT_MAJOR_VERSION}::Widgets)
  target_compile_definitions(vertexav_plugin_harness PRIVATE
    VERTEXAV_PLUGIN_PATH="$<TARGET_FILE:${module_name}>")
endif()
