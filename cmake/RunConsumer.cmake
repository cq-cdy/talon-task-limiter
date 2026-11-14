function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE result
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
set(common -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
  "-DCMAKE_BUILD_TYPE=${CONFIG}" "-DCMAKE_PREFIX_PATH=${relocated};${PREFIX_PATH}"
  "-Dtalon-status_DIR=${STATUS_DIR}"
  "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
  "-DCMAKE_CXX_FLAGS_DEBUG=${CXX_FLAGS_DEBUG}"
  "-DCMAKE_CXX_FLAGS_RELEASE=${CXX_FLAGS_RELEASE}"
  "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=${CXX_FLAGS_RELWITHDEBINFO}"
  "-DCMAKE_CXX_FLAGS_MINSIZEREL=${CXX_FLAGS_MINSIZEREL}")
foreach(setting IN ITEMS MSVC_RUNTIME OSX_ARCHITECTURES OSX_SYSROOT SYSROOT)
  if(${setting})
    if(setting STREQUAL "MSVC_RUNTIME")
      list(APPEND common "-DCMAKE_MSVC_RUNTIME_LIBRARY=${MSVC_RUNTIME}")
    else()
      list(APPEND common "-DCMAKE_${setting}=${${setting}}")
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
