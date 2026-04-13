cmake_minimum_required(VERSION 3.16)
project(obs-shot-presets VERSION 1.0.0 LANGUAGES C)

# --- Find OBS ---
# Set OBS_SOURCE_DIR to your OBS Studio source tree if not building in-tree
if(NOT TARGET OBS::libobs)
  find_package(libobs REQUIRED)
endif()

# --- Plugin shared library ---
add_library(obs-shot-presets MODULE
  src/plugin-main.c
  src/shot-presets-filter.c
  src/easing.c
)

target_include_directories(obs-shot-presets PRIVATE src)

target_link_libraries(obs-shot-presets PRIVATE OBS::libobs)

# Install into OBS plugin directory
if(WIN32)
  set(OBS_PLUGIN_DIR "$ENV{APPDATA}/obs-studio/plugins/obs-shot-presets/bin/64bit")
  set(OBS_DATA_DIR "$ENV{APPDATA}/obs-studio/plugins/obs-shot-presets/data")
else()
  set(OBS_PLUGIN_DIR "$ENV{HOME}/.config/obs-studio/plugins/obs-shot-presets/bin/64bit")
  set(OBS_DATA_DIR "$ENV{HOME}/.config/obs-studio/plugins/obs-shot-presets/data")
endif()

install(TARGETS obs-shot-presets DESTINATION "${OBS_PLUGIN_DIR}")
install(DIRECTORY data/ DESTINATION "${OBS_DATA_DIR}")
