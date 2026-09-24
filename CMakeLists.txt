cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CLATASHA_HUD_VERSION="${PROJECT_VERSION}")

set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/hud-window.cpp
    src/hud-window.hpp
    resources.qrc
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE shell32 winhttp)

  add_executable(clatasha-fps-helper src/fps-helper.cpp)
  target_compile_definitions(clatasha-fps-helper PRIVATE UNICODE _UNICODE NOMINMAX)
  target_link_libraries(clatasha-fps-helper PRIVATE advapi32)
  install(TARGETS clatasha-fps-helper RUNTIME DESTINATION "${CMAKE_PROJECT_NAME}/data")

  # Experimental x64 OpenGL fullscreen renderer probe. This is intentionally
  # separate from the OBS module so the first renderer-path test is isolated.
  add_library(clatasha-opengl-overlay SHARED src/opengl-overlay.cpp)
  target_compile_definitions(clatasha-opengl-overlay PRIVATE UNICODE _UNICODE NOMINMAX)
  target_link_libraries(clatasha-opengl-overlay PRIVATE opengl32)
  set_target_properties(
    clatasha-opengl-overlay
    PROPERTIES
      OUTPUT_NAME "clatasha-opengl-overlay"
  )
  install(
    TARGETS clatasha-opengl-overlay
    RUNTIME DESTINATION "${CMAKE_PROJECT_NAME}/data"
  )
endif()
