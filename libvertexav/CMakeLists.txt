cmake_minimum_required(VERSION 3.16)
project(libvertexav VERSION 0.6.0 LANGUAGES CXX)

# Reference C++/Qt implementation of VertexAV Link 0.6.
# Depends only on QtCore and QtNetwork (Qt 6, or Qt 5.15).

option(VERTEXAV_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(VERTEXAV_BUILD_TOOLS "Build vxdump and vxemit" ${PROJECT_IS_TOP_LEVEL})
set(VERTEXAV_QT_MAJOR "" CACHE STRING "Force Qt major version (5 or 6); empty = auto")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT VERTEXAV_QT_MAJOR)
  if(TARGET Qt6::Core)
    set(VERTEXAV_QT_MAJOR 6)
  elseif(TARGET Qt5::Core)
    set(VERTEXAV_QT_MAJOR 5)
  else()
    find_package(Qt6 COMPONENTS Core QUIET)
    if(Qt6_FOUND)
      set(VERTEXAV_QT_MAJOR 6)
    else()
      set(VERTEXAV_QT_MAJOR 5)
    endif()
  endif()
endif()
if(VERTEXAV_QT_MAJOR EQUAL 5)
  find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network)
else()
  find_package(Qt6 REQUIRED COMPONENTS Core Network)
endif()

add_library(vertexav STATIC
  include/vertexav/emitter.h
  include/vertexav/mdns.h
  include/vertexav/mixanalyzer.h
  include/vertexav/protocol.h
  include/vertexav/receiver.h
  include/vertexav/security.h
  include/vertexav/tempo.h
  include/vertexav/vertexav.h
  include/vertexav/websocket.h
  src/emitter.cpp
  src/mdns.cpp
  src/mixanalyzer.cpp
  src/protocol.cpp
  src/receiver.cpp
  src/security.cpp
  src/tempo.cpp
  src/websocket.cpp
)
add_library(vertexav::vertexav ALIAS vertexav)
target_include_directories(vertexav PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vertexav PUBLIC Qt${VERTEXAV_QT_MAJOR}::Core Qt${VERTEXAV_QT_MAJOR}::Network)
set_target_properties(vertexav PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(vertexav PRIVATE /W4)
else()
  target_compile_options(vertexav PRIVATE -Wall -Wextra)
endif()

if(VERTEXAV_BUILD_TOOLS)
  add_executable(vxdump tools/vxdump.cpp)
  target_link_libraries(vxdump PRIVATE vertexav)
  add_executable(vxemit tools/vxemit.cpp)
  target_link_libraries(vxemit PRIVATE vertexav)
endif()

if(VERTEXAV_BUILD_TESTS)
  enable_testing()
  find_package(Qt${VERTEXAV_QT_MAJOR} REQUIRED COMPONENTS Test)
  add_executable(vertexav_tests tests/test_vertexav.cpp)
  target_link_libraries(vertexav_tests PRIVATE vertexav Qt${VERTEXAV_QT_MAJOR}::Test)
  add_test(NAME vertexav_tests COMMAND vertexav_tests)
endif()
