cmake_minimum_required(VERSION 3.20)
project(talon-task-limiter VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(GenerateExportHeader)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(talon_task_limiter_top_level ON)
else()
  set(talon_task_limiter_top_level OFF)
endif()
option(TALON_TASK_LIMITER_BUILD_TESTS "Build tests" ${talon_task_limiter_top_level})
option(TALON_TASK_LIMITER_BUILD_EXAMPLES "Build examples" ${talon_task_limiter_top_level})
option(TALON_TASK_LIMITER_BUILD_BENCHMARKS "Build throughput benchmark" OFF)
option(TALON_TASK_LIMITER_FETCH_DEPENDENCIES "Fetch missing pinned dependencies" ON)
option(TALON_TASK_LIMITER_STRICT_WARNINGS "Treat warnings as errors on own targets" ON)
set(TALON_TASK_LIMITER_SANITIZER "none" CACHE STRING "none, address, or thread")
set_property(CACHE TALON_TASK_LIMITER_SANITIZER PROPERTY STRINGS none address thread)
if(NOT TALON_TASK_LIMITER_SANITIZER MATCHES "^(none|address|thread)$")
  message(FATAL_ERROR "TALON_TASK_LIMITER_SANITIZER must be none, address, or thread")
endif()
if(NOT TALON_TASK_LIMITER_SANITIZER STREQUAL "none" AND
   (MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang"))
  message(FATAL_ERROR "Sanitizers require GCC or Clang on a supported platform")
endif()

include(cmake/Dependencies.cmake)
find_package(Threads REQUIRED)

add_library(talon_task_limiter src/concurrent_task_limiter.cc)
add_library(talon::task_limiter ALIAS talon_task_limiter)
set_target_properties(talon_task_limiter PROPERTIES
  EXPORT_NAME task_limiter VERSION ${PROJECT_VERSION} SOVERSION 0
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
generate_export_header(talon_task_limiter
  EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/talon/task_limiter/export.h")
get_target_property(talon_task_limiter_type talon_task_limiter TYPE)
if(talon_task_limiter_type STREQUAL "STATIC_LIBRARY")
  target_compile_definitions(talon_task_limiter PUBLIC TALON_TASK_LIMITER_STATIC_DEFINE)
endif()
target_include_directories(talon_task_limiter PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(talon_task_limiter PUBLIC cxx_std_17)
target_link_libraries(talon_task_limiter PUBLIC talon::status Threads::Threads)

function(talon_task_limiter_target target)
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
  if(TALON_TASK_LIMITER_STRICT_WARNINGS)
    if(MSVC)
      target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
      target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
  endif()
  if(TALON_TASK_LIMITER_SANITIZER STREQUAL "address")
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  elseif(TALON_TASK_LIMITER_SANITIZER STREQUAL "thread")
    target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=thread)
  endif()
endfunction()
talon_task_limiter_target(talon_task_limiter)

if(TALON_TASK_LIMITER_BUILD_TESTS)
  enable_testing()
  include(cmake/Tests.cmake)
endif()
if(TALON_TASK_LIMITER_BUILD_EXAMPLES)
  add_executable(talon_task_limiter_example examples/basic.cc)
  target_link_libraries(talon_task_limiter_example PRIVATE talon::task_limiter)
  talon_task_limiter_target(talon_task_limiter_example)
  if(TALON_TASK_LIMITER_BUILD_TESTS)
    add_test(NAME limiter.example COMMAND talon_task_limiter_example)
  endif()
endif()
if(TALON_TASK_LIMITER_BUILD_BENCHMARKS)
  add_executable(talon_task_limiter_benchmark benchmarks/throughput.cc)
  target_link_libraries(talon_task_limiter_benchmark PRIVATE talon::task_limiter)
  talon_task_limiter_target(talon_task_limiter_benchmark)
endif()

install(TARGETS talon_task_limiter EXPORT talon-task-limiter-targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/talon/task_limiter/export.h"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/talon/task_limiter)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/talon-task-limiter)
install(EXPORT talon-task-limiter-targets NAMESPACE talon::
  FILE talon-task-limiter-targets.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/talon-task-limiter)
configure_package_config_file(cmake/talon-task-limiter-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/talon-task-limiter-config.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/talon-task-limiter)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/talon-task-limiter-config-version.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/talon-task-limiter-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/talon-task-limiter-config-version.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/talon-task-limiter)
