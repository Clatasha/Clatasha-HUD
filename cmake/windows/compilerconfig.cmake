include_guard(GLOBAL)

include(compiler_common)

set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)

if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION VERSION_LESS 10.0.20348)
  message(FATAL_ERROR "OBS requires Windows SDK 10.0.20348.0 or newer.")
endif()

add_compile_options(
  /W3
  /utf-8
  /MP
  /Zc:__cplusplus
  /Zc:preprocessor
  /permissive-
)

add_compile_definitions(
  UNICODE
  _UNICODE
  _CRT_SECURE_NO_WARNINGS
  _CRT_NONSTDC_NO_WARNINGS
)

add_link_options(/DEBUG)
