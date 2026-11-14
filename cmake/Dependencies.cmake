include(FetchContent)

if(NOT TARGET talon::status)
  find_package(talon-status 0.1 CONFIG QUIET)
endif()
if(NOT TARGET talon::status)
  if(NOT TALON_TASK_LIMITER_FETCH_DEPENDENCIES)
    message(FATAL_ERROR "talon-status was not found. Install it, provide talon::status, or enable TALON_TASK_LIMITER_FETCH_DEPENDENCIES")
  endif()
  # Deliberately pinned; never track a moving branch or modify sibling checkouts.
  FetchContent_Declare(talon_status
    GIT_REPOSITORY https://github.com/cq-cdy/talon-status.git
    GIT_TAG aaf333da1e5c9f494d5750f52fd0838aeb4e4205)
  FetchContent_MakeAvailable(talon_status)
endif()

if(TALON_TASK_LIMITER_BUILD_TESTS AND NOT TARGET GTest::gtest_main)
  find_package(GTest 1.11 CONFIG QUIET)
  if(NOT TARGET GTest::gtest_main)
    if(NOT TALON_TASK_LIMITER_FETCH_DEPENDENCIES)
      message(FATAL_ERROR "GoogleTest >= 1.11 is required to build tests")
    endif()
    # Set normal scoped defaults, without overwriting a parent's cache choices.
    set(INSTALL_GTEST OFF)
    set(gtest_force_shared_crt ON)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571)
    FetchContent_MakeAvailable(googletest)
  endif()
endif()
