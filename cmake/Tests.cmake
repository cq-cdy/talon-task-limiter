include(GoogleTest)
add_executable(talon_task_limiter_test
  tests/api_test.cc tests/lifecycle_test.cc tests/concurrency_test.cc)
target_link_libraries(talon_task_limiter_test PRIVATE talon::task_limiter GTest::gtest_main)
talon_task_limiter_target(talon_task_limiter_test)
gtest_discover_tests(talon_task_limiter_test DISCOVERY_MODE PRE_TEST
  TEST_PREFIX "limiter." PROPERTIES TIMEOUT 90)

# Allocation replacement is isolated from ASan's allocator interposition.
if(TALON_TASK_LIMITER_SANITIZER STREQUAL "none")
  add_executable(talon_task_limiter_allocation_test tests/allocation_test.cc)
  target_link_libraries(talon_task_limiter_allocation_test PRIVATE talon::task_limiter GTest::gtest_main)
  talon_task_limiter_target(talon_task_limiter_allocation_test)
  gtest_discover_tests(talon_task_limiter_allocation_test DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "limiter." PROPERTIES TIMEOUT 90)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(talon_task_limiter_thread_failure_test tests/thread_start_failure_test.cc)
    target_link_libraries(talon_task_limiter_thread_failure_test PRIVATE
      talon::task_limiter GTest::gtest_main ${CMAKE_DL_LIBS})
    talon_task_limiter_target(talon_task_limiter_thread_failure_test)
    gtest_discover_tests(talon_task_limiter_thread_failure_test DISCOVERY_MODE PRE_TEST
      TEST_PREFIX "limiter." PROPERTIES TIMEOUT 90)
  endif()
endif()

# Compile public headers in independent translation units, then link them.
add_executable(talon_task_limiter_header_test tests/header_main.cc tests/header_other.cc)
target_link_libraries(talon_task_limiter_header_test PRIVATE talon::task_limiter)
talon_task_limiter_target(talon_task_limiter_header_test)
add_test(NAME limiter.headers COMMAND talon_task_limiter_header_test)

if(TALON_TASK_LIMITER_SANITIZER STREQUAL "none" AND NOT CMAKE_CROSSCOMPILING)
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
