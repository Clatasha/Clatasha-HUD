# Plugin bootstrap module

include_guard(GLOBAL)

set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO RelWithDebInfo Release MinSizeRel None "")
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL MinSizeRel Release RelWithDebInfo None "")
set(CMAKE_MAP_IMPORTED_CONFIG_RELEASE Release RelWithDebInfo MinSizeRel None "")

if("${CMAKE_CURRENT_BINARY_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
  message(FATAL_ERROR "In-source builds are not supported.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/common")

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" buildspec)

string(JSON _name GET ${buildspec} name)
string(JSON _website GET ${buildspec} website)
string(JSON _author GET ${buildspec} author)
string(JSON _email GET ${buildspec} email)
string(JSON _version GET ${buildspec} version)
string(JSON _bundleId GET ${buildspec} platformConfig macos bundleId)

set(PLUGIN_AUTHOR ${_author})
set(PLUGIN_WEBSITE ${_website})
set(PLUGIN_EMAIL ${_email})
set(PLUGIN_VERSION ${_version})
set(MACOS_BUNDLEID ${_bundleId})

string(REPLACE "." ";" _version_canonical "${_version}")
list(GET _version_canonical 0 PLUGIN_VERSION_MAJOR)
list(GET _version_canonical 1 PLUGIN_VERSION_MINOR)
list(GET _version_canonical 2 PLUGIN_VERSION_PATCH)
unset(_version_canonical)

include(buildnumber)
include(osconfig)

set(CMAKE_EXPORT_PACKAGE_REGISTRY FALSE)
set(CMAKE_INCLUDE_CURRENT_DIR TRUE)
