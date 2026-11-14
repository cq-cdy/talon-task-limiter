cmake_minimum_required(VERSION 3.20)

# Validate cleanup ownership before touching any generated directories. This
# private test driver must fail safely when invoked without CTest's arguments.
foreach(required IN ITEMS SOURCE_DIR BINARY_DIR CXX_COMPILER GENERATOR)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()
if(NOT IS_ABSOLUTE "${BINARY_DIR}" OR
   NOT EXISTS "${BINARY_DIR}/talon-task-limiter-config.cmake" OR
   NOT EXISTS "${BINARY_DIR}/cmake_install.cmake")
  message(FATAL_ERROR "BINARY_DIR must be an existing talon-task-limiter build directory")
endif()
if(NOT EXISTS "${SOURCE_DIR}/tests/consumer/CMakeLists.txt")
  message(FATAL_ERROR "SOURCE_DIR must contain the task-limiter consumer fixture")
endif()

function(run)
  # ARGV is itself a CMake list. Re-escape literal semicolons in each original
  # argument before forwarding, otherwise a -D value can become extra argv.
  set(command)
  math(EXPR last_argument "${ARGC} - 1")
  foreach(index RANGE 0 ${last_argument})
    string(REPLACE ";" "\\;" argument "${ARGV${index}}")
    list(APPEND command "${argument}")
  endforeach()
  execute_process(COMMAND ${command} RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Command failed (${result}): ${ARGV}\n${output}\n${error}")
  endif()
endfunction()

set(stage "${BINARY_DIR}/package-check/stage")
set(relocated "${BINARY_DIR}/package-check/relocated")
file(REMOVE_RECURSE "${stage}" "${relocated}")
run("${CMAKE_COMMAND}" --install "${BINARY_DIR}" --prefix "${stage}" --config "${CONFIG}")
file(RENAME "${stage}" "${relocated}")
if(WIN32)
  set(ENV{PATH} "${relocated}/bin;$ENV{PATH}")
endif()
set(prefix_path "${relocated};${PREFIX_PATH}")
list(FILTER prefix_path EXCLUDE REGEX "^$")
# Exercise a real CMake list, including a path containing spaces.
list(APPEND prefix_path "${BINARY_DIR}/package-check/unused prefix")
list(LENGTH prefix_path expected_prefix_count)
string(REPLACE ";" "\\;" escaped_prefix_path "${prefix_path}")
set(common -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
  "-DCMAKE_BUILD_TYPE=${CONFIG}" "-DCMAKE_PREFIX_PATH=${escaped_prefix_path}"
  "-DEXPECTED_PREFIX_COUNT=${expected_prefix_count}"
  "-Dtalon-status_DIR=${STATUS_DIR}"
  "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
  "-DCMAKE_CXX_FLAGS_DEBUG=${CXX_FLAGS_DEBUG}"
  "-DCMAKE_CXX_FLAGS_RELEASE=${CXX_FLAGS_RELEASE}"
  "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=${CXX_FLAGS_RELWITHDEBINFO}"
  "-DCMAKE_CXX_FLAGS_MINSIZEREL=${CXX_FLAGS_MINSIZEREL}"
  "-DCMAKE_EXE_LINKER_FLAGS=${EXE_LINKER_FLAGS}"
  "-DCMAKE_SHARED_LINKER_FLAGS=${SHARED_LINKER_FLAGS}")
foreach(setting IN ITEMS MSVC_RUNTIME OSX_ARCHITECTURES OSX_SYSROOT SYSROOT)
  if(${setting})
    if(setting STREQUAL "MSVC_RUNTIME")
      list(APPEND common "-DCMAKE_MSVC_RUNTIME_LIBRARY=${MSVC_RUNTIME}")
    else()
      string(REPLACE ";" "\\;" escaped_setting "${${setting}}")
      list(APPEND common "-DCMAKE_${setting}=${escaped_setting}")
    endif()
  endif()
endforeach()
if(PLATFORM)
  list(APPEND common -A "${PLATFORM}")
endif()
if(TOOLSET)
  list(APPEND common -T "${TOOLSET}")
endif()
if(TOOLCHAIN)
  list(APPEND common "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}")
endif()

foreach(mode IN ITEMS installed source)
  set(build "${BINARY_DIR}/package-check/${mode}")
  if(mode STREQUAL "installed")
    set(extra -DCHECK_COMPILE_FAILURES=ON)
  else()
    set(extra "-DLIMITER_SOURCE_DIR=${SOURCE_DIR}" -DCHECK_COMPILE_FAILURES=OFF)
  endif()
  run("${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/consumer" -B "${build}" ${common} ${extra})
  run("${CMAKE_COMMAND}" --build "${build}" --config "${CONFIG}" --parallel 2)
  run("${CMAKE_CTEST_COMMAND}" --test-dir "${build}" -C "${CONFIG}" --output-on-failure)
endforeach()
