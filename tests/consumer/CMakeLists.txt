cmake_minimum_required(VERSION 3.20)
project(task-limiter-consumer LANGUAGES CXX)
if(DEFINED EXPECTED_PREFIX_COUNT)
  list(LENGTH CMAKE_PREFIX_PATH actual_prefix_count)
  if(NOT actual_prefix_count EQUAL EXPECTED_PREFIX_COUNT)
    message(FATAL_ERROR "CMAKE_PREFIX_PATH list was not forwarded intact")
  endif()
endif()
set(CMAKE_CXX_STANDARD 17 CACHE STRING "Consumer language mode")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(LIMITER_SOURCE_DIR)
  set(TALON_TASK_LIMITER_BUILD_TESTS OFF)
  set(TALON_TASK_LIMITER_BUILD_EXAMPLES OFF)
  set(TALON_TASK_LIMITER_FETCH_DEPENDENCIES OFF)
  add_subdirectory("${LIMITER_SOURCE_DIR}" limiter)
else()
  find_package(talon-task-limiter 0.1 CONFIG REQUIRED)
endif()
add_executable(consumer main.cc other.cc)
target_link_libraries(consumer PRIVATE talon::task_limiter)
enable_testing()
add_test(NAME consumer.run COMMAND consumer)

if(CHECK_COMPILE_FAILURES)
  # Prove include paths, dependencies, toolchain and link target work before
  # accepting any negative result as an expected diagnostic.
  try_compile(baseline_ok "${CMAKE_CURRENT_BINARY_DIR}/baseline"
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/main.cc" "${CMAKE_CURRENT_SOURCE_DIR}/other.cc"
    LINK_LIBRARIES talon::task_limiter OUTPUT_VARIABLE baseline_output)
  if(NOT baseline_ok)
    message(FATAL_ERROR "Compile-failure baseline is broken: ${baseline_output}")
  endif()
  foreach(case IN ITEMS not_callable lvalue_only throwing_destructor
      rvalue_result copy_executor throwing_result_destructor immovable_result
      throwing_argument_destructor move_only_lvalue borrow_requires_ref
      noncopyable_callable_lvalue wrong_arity const_borrow_mutation
      move_executor assign_executor move_assign_executor)
    try_compile(unexpected_success "${CMAKE_CURRENT_BINARY_DIR}/negative/${case}"
      SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../compile_fail/${case}.cc"
      LINK_LIBRARIES talon::task_limiter OUTPUT_VARIABLE diagnostic)
    if(unexpected_success)
      message(FATAL_ERROR "Expected ${case}.cc to fail compilation")
    endif()
    if(case MATCHES "^(not_callable|lvalue_only|borrow_requires_ref|wrong_arity|const_borrow_mutation)$")
      set(expected "Task must be invocable")
    elseif(case MATCHES "^(throwing_destructor|throwing_argument_destructor)$")
      set(expected "Task captures must have noexcept destructors")
    elseif(case STREQUAL "rvalue_result")
      set(expected "std::future does not support rvalue reference results")
    elseif(case MATCHES "^(throwing_result_destructor|immovable_result)$")
      set(expected "Future value results must be move-constructible")
    elseif(case STREQUAL "move_only_lvalue")
      # The unavailable tuple construction is diagnosed differently by each
      # standard library; all compilers must reject copying the move-only arg.
      set(expected "(deleted|no matching|no suitable|cannot convert|no overloaded)")
    else()
      set(expected "deleted")
    endif()
    if(NOT diagnostic MATCHES "${expected}")
      message(FATAL_ERROR "Wrong failure for ${case}: ${diagnostic}")
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/negative/${case}.log" "${diagnostic}")
  endforeach()
endif()
