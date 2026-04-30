# --- Detect if the plugin is build out of tree or not ---
if(CMAKE_PROJECT_NAME STREQUAL "obs-studio")
  set(BUILD_OUT_OF_TREE OFF)
else()
  set(BUILD_OUT_OF_TREE ON)
  cmake_minimum_required(VERSION 3.16...3.26)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version} LANGUAGES C CXX)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${PROJECT_NAME} MODULE)

target_link_libraries(${PROJECT_NAME} PRIVATE OBS::libobs)

if(BUILD_OUT_OF_TREE)
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
  find_package(Qt6 REQUIRED COMPONENTS Widgets)
  target_link_libraries(${PROJECT_NAME} PRIVATE OBS::obs-frontend-api Qt6::Widgets)
else()
  find_package(Qt6 REQUIRED COMPONENTS Widgets)
  target_link_libraries(${PROJECT_NAME} PRIVATE OBS::frontend-api Qt6::Widgets)
endif()

# --- BMD ATEM Switchers SDK (Windows only) ---
# In-process COM control of the ATEM Mini for shot-preset → input
# switching. The Windows SDK ships only an .idl; we MIDL-compile it into
# BMDSwitcherAPI.h + BMDSwitcherAPI_i.c at build time. No static .lib to
# link — IBMDSwitcher* is loaded at runtime via CoCreateInstance against
# BMDSwitcherAPI64.dll registered by the ATEM installer.
if(WIN32)
  if(NOT DEFINED ATEM_SDK_INCLUDE_DIR)
    set(ATEM_SDK_INCLUDE_DIR
        "C:/Program Files (x86)/Blackmagic Design/Blackmagic ATEM Switchers/Developer SDK/Windows/include")
  endif()

  set(_atem_idl "${ATEM_SDK_INCLUDE_DIR}/BMDSwitcherAPI.idl")
  if(EXISTS "${_atem_idl}")
    set(_atem_gen "${CMAKE_CURRENT_BINARY_DIR}/atem_idl_gen")
    file(MAKE_DIRECTORY "${_atem_gen}")
    set(_atem_h "${_atem_gen}/BMDSwitcherAPI.h")
    set(_atem_i "${_atem_gen}/BMDSwitcherAPI_i.c")

    find_program(MIDL_EXECUTABLE midl
      HINTS
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.19041.0/x64"
      REQUIRED)

    add_custom_command(
      OUTPUT "${_atem_h}" "${_atem_i}"
      COMMAND "${MIDL_EXECUTABLE}"
              /nologo
              /header BMDSwitcherAPI.h
              /iid    BMDSwitcherAPI_i.c
              /out    "${_atem_gen}"
              "${_atem_idl}"
      DEPENDS "${_atem_idl}"
      COMMENT "MIDL: BMDSwitcherAPI.idl -> BMDSwitcherAPI.h + _i.c"
      VERBATIM)

    target_sources(${PROJECT_NAME} PRIVATE
      atem-control.cpp
      atem-control.h
      "${_atem_h}"
      "${_atem_i}")
    target_include_directories(${PROJECT_NAME} PRIVATE
      "${ATEM_SDK_INCLUDE_DIR}"
      "${_atem_gen}")
    target_link_libraries(${PROJECT_NAME} PRIVATE Ole32 OleAut32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE FNN_ATEM_AVAILABLE=1)
    message(STATUS "Shot Presets: ATEM SDK found at ${ATEM_SDK_INCLUDE_DIR}")
  else()
    # SDK missing — build the plugin without ATEM control. Calls to
    # atem_set_program_input become no-ops via the stub in atem-control.cpp.
    target_sources(${PROJECT_NAME} PRIVATE atem-control.cpp atem-control.h)
    message(WARNING
      "Shot Presets: ATEM SDK not found at ${ATEM_SDK_INCLUDE_DIR}\n"
      "  ATEM input switching will be a no-op. Install the BMD ATEM\n"
      "  Switchers software, or pass -DATEM_SDK_INCLUDE_DIR=<path>.")
  endif()
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  AUTOMOC ON)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/version.h)

target_sources(${PROJECT_NAME} PRIVATE
  shot-presets-filter.c
  shot-presets-dock.cpp
  shot-presets-dock.hpp
  easing.c
  easing.h
  version.h)

if(BUILD_OUT_OF_TREE)
  set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
else()
  set_target_properties_obs(${PROJECT_NAME} PROPERTIES FOLDER "plugins/shot-presets" PREFIX "")
endif()
