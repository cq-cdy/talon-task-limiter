include(GoogleTest)
add_executable(talon_task_limiter_test
  tests/api_test.cc tests/lifecycle_test.cc tests/concurrency_test.cc
  tests/history_test.cc tests/reservation_matrix_test.cc
  tests/reentrancy_stress_test.cc tests/invocation_test.cc
  tests/macro_integration_test.cc)
target_link_libraries(talon_task_limiter_test PRIVATE talon::task_limiter GTest::gtest_main)
talon_task_limiter_target(talon_task_limiter_test)
gtest_discover_tests(talon_task_limiter_test DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES
  TEST_PREFIX "limiter." PROPERTIES TIMEOUT 90)

add_executable(talon_task_limiter_workload_test
  tests/bulk_workload_test.cc tests/matrix_workload_test.cc
  tests/dag_workload_test.cc tests/continuation_workload_test.cc)
target_link_libraries(talon_task_limiter_workload_test PRIVATE
  talon::task_limiter GTest::gtest_main)
talon_task_limiter_target(talon_task_limiter_workload_test)
gtest_discover_tests(talon_task_limiter_workload_test DISCOVERY_MODE PRE_TEST
  NO_PRETTY_VALUES TEST_PREFIX "workload."
  PROPERTIES TIMEOUT 300 LABELS workload RUN_SERIAL TRUE)

# Allocation replacement is isolated from ASan's allocator interposition.
if(TALON_TASK_LIMITER_SANITIZER STREQUAL "none")
  add_executable(talon_task_limiter_allocation_test
    tests/allocation_test.cc tests/allocation_failure.cc)
  # Keep allocation interposition opaque even if a parent enables global LTO.
  set_target_properties(talon_task_limiter_allocation_test PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION FALSE)
  target_link_libraries(talon_task_limiter_allocation_test PRIVATE talon::task_limiter GTest::gtest_main)
  talon_task_limiter_target(talon_task_limiter_allocation_test)
  gtest_discover_tests(talon_task_limiter_allocation_test DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "limiter." PROPERTIES TIMEOUT 90)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(talon_task_limiter_thread_failure_test tests/thread_start_failure_test.cc)
    target_link_libraries(talon_task_limiter_thread_failure_test PRIVATE
      talon::task_limiter GTest::gtest_main ${CMAKE_DL_LIBS})
    talon_task_limiter_target(talon_task_limiter_thread_failure_test)
    gtest_discover_tests(talon_task_limiter_thread_failure_test DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES
      TEST_PREFIX "limiter." PROPERTIES TIMEOUT 90)
  endif()
endif()

# Compile public headers in independent translation units, then link them.
add_executable(talon_task_limiter_header_test
  tests/header_main.cc tests/header_other.cc tests/header_macros.cc)
target_link_libraries(talon_task_limiter_header_test PRIVATE talon::task_limiter)
talon_task_limiter_target(talon_task_limiter_header_test)
add_test(NAME limiter.headers COMMAND talon_task_limiter_header_test)

set(macro_test_standard 17)
if(CMAKE_CXX_STANDARD GREATER macro_test_standard)
  set(macro_test_standard "${CMAKE_CXX_STANDARD}")
endif()
get_target_property(status_features talon::status INTERFACE_COMPILE_FEATURES)
foreach(feature IN LISTS status_features)
  if(feature MATCHES "^cxx_std_([0-9]+)$" AND CMAKE_MATCH_1 GREATER macro_test_standard)
    set(macro_test_standard "${CMAKE_MATCH_1}")
  endif()
endforeach()
add_test(NAME limiter.macro_odr
  COMMAND ${CMAKE_COMMAND}
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
    "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
    "-DMSVC_MODE=${MSVC}"
    "-DSYSROOT=${CMAKE_SYSROOT}"
    "-DOSX_SYSROOT=${CMAKE_OSX_SYSROOT}"
    "-DSTANDARD=${macro_test_standard}"
    "-DSTATUS_INCLUDES=$<TARGET_PROPERTY:talon::status,INTERFACE_INCLUDE_DIRECTORIES>"
    "-DSTATUS_DEFINITIONS=$<TARGET_PROPERTY:talon::status,INTERFACE_COMPILE_DEFINITIONS>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckMacroOdr.cmake")
set_tests_properties(limiter.macro_odr PROPERTIES TIMEOUT 60)

if(NOT TALON_TASK_LIMITER_SANITIZER STREQUAL "none")
  # Deliberately omit talon_task_limiter_target(): this consumer must link using
  # only the exported usage requirements of the instrumented library.
  add_executable(talon_task_limiter_sanitizer_consumer
    tests/consumer/main.cc tests/consumer/other.cc)
  target_link_libraries(talon_task_limiter_sanitizer_consumer PRIVATE talon::task_limiter)
  set_target_properties(talon_task_limiter_sanitizer_consumer PROPERTIES CXX_EXTENSIONS OFF)
  add_test(NAME limiter.sanitizer_consumer COMMAND talon_task_limiter_sanitizer_consumer)
endif()

if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME limiter.package
    COMMAND ${CMAKE_COMMAND}
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DGENERATOR=${CMAKE_GENERATOR}"
      "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
      "-DTOOLSET=${CMAKE_GENERATOR_TOOLSET}"
      "-DTOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}"
      "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
      "-DCXX_FLAGS_DEBUG=${CMAKE_CXX_FLAGS_DEBUG}"
      "-DCXX_FLAGS_RELEASE=${CMAKE_CXX_FLAGS_RELEASE}"
      "-DCXX_FLAGS_RELWITHDEBINFO=${CMAKE_CXX_FLAGS_RELWITHDEBINFO}"
      "-DCXX_FLAGS_MINSIZEREL=${CMAKE_CXX_FLAGS_MINSIZEREL}"
      "-DEXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
      "-DSHARED_LINKER_FLAGS=${CMAKE_SHARED_LINKER_FLAGS}"
      "-DMSVC_RUNTIME=${CMAKE_MSVC_RUNTIME_LIBRARY}"
      "-DOSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
      "-DOSX_SYSROOT=${CMAKE_OSX_SYSROOT}"
      "-DSYSROOT=${CMAKE_SYSROOT}"
      "-DPREFIX_PATH=${CMAKE_PREFIX_PATH}"
      "-DSTATUS_DIR=${talon-status_DIR}"
      "-DCONFIG=$<CONFIG>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunConsumer.cmake")
  set_tests_properties(limiter.package PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
endif()
