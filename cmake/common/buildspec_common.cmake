include_guard(GLOBAL)

function(_check_deps_version version)
  set(found FALSE)
  foreach(path IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${path}/share/obs-deps/VERSION")
      if(dependency STREQUAL qt6 AND NOT EXISTS "${path}/lib/cmake/Qt6/Qt6Config.cmake")
        continue()
      endif()
      file(READ "${path}/share/obs-deps/VERSION" _check_version)
      string(REPLACE "\n" "" _check_version "${_check_version}")
      string(REPLACE "-" "." _check_version "${_check_version}")
      string(REPLACE "-" "." version "${version}")
      if(NOT _check_version VERSION_LESS version)
        set(found TRUE)
        break()
      endif()
    endif()
  endforeach()
  return(PROPAGATE found CMAKE_PREFIX_PATH)
endfunction()

function(_setup_obs_studio)
  if(NOT libobs_DIR)
    set(_is_fresh --fresh)
  endif()

  set(_cmake_generator "${CMAKE_GENERATOR}")
  set(_cmake_arch "-A ${arch},version=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
  set(_cmake_extra "-DCMAKE_SYSTEM_VERSION=${CMAKE_SYSTEM_VERSION} -DCMAKE_ENABLE_SCRIPTING=OFF")

  message(STATUS "Configure ${label} (${arch})")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${dependencies_dir}/${_obs_destination}" -B
      "${dependencies_dir}/${_obs_destination}/build_${arch}" -G ${_cmake_generator} "${_cmake_arch}"
      -DOBS_CMAKE_VERSION:STRING=3.0.0 -DENABLE_PLUGINS:BOOL=OFF -DENABLE_FRONTEND:BOOL=OFF
      -DOBS_VERSION_OVERRIDE:STRING=${_obs_version} "-DCMAKE_PREFIX_PATH='${CMAKE_PREFIX_PATH}'"
      ${_is_fresh} ${_cmake_extra}
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET
  )

  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build build_${arch} --target obs-frontend-api --config Debug --parallel
    WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET
  )

  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build build_${arch} --target obs-frontend-api --config Release --parallel
    WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET
  )

  execute_process(
    COMMAND "${CMAKE_COMMAND}" --install build_${arch} --component Development --config Debug --prefix "${dependencies_dir}"
    WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET
  )

  execute_process(
    COMMAND "${CMAKE_COMMAND}" --install build_${arch} --component Development --config Release --prefix "${dependencies_dir}"
    WORKING_DIRECTORY "${dependencies_dir}/${_obs_destination}"
    COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_QUIET
  )
endfunction()

function(_check_dependencies)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" buildspec)
  string(JSON dependency_data GET ${buildspec} dependencies)

  foreach(dependency IN LISTS dependencies_list)
    string(JSON data GET ${dependency_data} ${dependency})
    string(JSON version GET ${data} version)
    string(JSON hash GET ${data} hashes ${platform})
    string(JSON url GET ${data} baseUrl)
    string(JSON label GET ${data} label)

    set(file "${${dependency}_filename}")
    set(destination "${${dependency}_destination}")
    string(REPLACE "VERSION" "${version}" file "${file}")
    string(REPLACE "VERSION" "${version}" destination "${destination}")
    string(REPLACE "ARCH" "${arch}" file "${file}")
    string(REPLACE "ARCH" "${arch}" destination "${destination}")
    string(REPLACE "_REVISION" "" file "${file}")
    string(REPLACE "-REVISION" "" file "${file}")

    if(dependency STREQUAL obs-studio)
      set(url "${url}/${file}")
    else()
      set(url "${url}/${version}/${file}")
    endif()

    if(NOT EXISTS "${dependencies_dir}/${file}")
      message(STATUS "Downloading ${url}")
      file(DOWNLOAD "${url}" "${dependencies_dir}/${file}" STATUS download_status EXPECTED_HASH SHA256=${hash})
      list(GET download_status 0 error_code)
      list(GET download_status 1 error_message)
      if(error_code GREATER 0)
        file(REMOVE "${dependencies_dir}/${file}")
        message(FATAL_ERROR "Unable to download ${url}: ${error_message}")
      endif()
    endif()

    if(NOT EXISTS "${dependencies_dir}/${destination}")
      file(MAKE_DIRECTORY "${dependencies_dir}/${destination}")
      if(dependency STREQUAL obs-studio)
        file(ARCHIVE_EXTRACT INPUT "${dependencies_dir}/${file}" DESTINATION "${dependencies_dir}")
      else()
        file(ARCHIVE_EXTRACT INPUT "${dependencies_dir}/${file}" DESTINATION "${dependencies_dir}/${destination}")
      endif()
    endif()

    if(dependency STREQUAL prebuilt OR dependency STREQUAL qt6)
      list(APPEND CMAKE_PREFIX_PATH "${dependencies_dir}/${destination}")
    elseif(dependency STREQUAL obs-studio)
      set(_obs_version ${version})
      set(_obs_destination "${destination}")
      list(APPEND CMAKE_PREFIX_PATH "${dependencies_dir}")
    endif()
  endforeach()

  list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
  set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} CACHE PATH "CMake prefix search path" FORCE)

  _setup_obs_studio()
endfunction()
