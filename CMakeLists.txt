cmake_minimum_required(VERSION 3.22)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)

project(nyx)

add_subdirectory(vendor)
add_subdirectory(tools)

# TODO: make optional
add_subdirectory(dolos)

set(NYX_SOURCES
  src/nyx/gui/canvas.cc
  src/nyx/gui/colors.cc
  src/nyx/gui/combo.cc
  src/nyx/gui/common.cc
  src/nyx/gui/image.cc
  src/nyx/gui/input.cc
  src/nyx/gui/io.cc
  src/nyx/gui/layout.cc
  src/nyx/gui/listbox.cc
  src/nyx/gui/menus.cc
  src/nyx/gui/panel.cc
  src/nyx/gui/plotting.cc
  src/nyx/gui/popups.cc
  src/nyx/gui/selectable.cc
  src/nyx/gui/slider.cc
  src/nyx/gui/stack.cc
  src/nyx/gui/tables.cc
  src/nyx/gui/tabs.cc
  src/nyx/gui/text.cc
  src/nyx/gui/tooltip.cc
  src/nyx/gui/trees.cc
  src/nyx/gui/widget.cc
  src/nyx/gui/widget_binding.cc
  src/nyx/gui/widget_manager.cc

  src/nyx/base_object.cc
  src/nyx/builtins.cc
  src/nyx/console_binding.cc
  src/nyx/env.cc
  src/nyx/errors.cc
  src/nyx/extension.cc
  src/nyx/game_lock.cc
  src/nyx/imgui_draw_context.cc
  src/nyx/imgui_input_event.cc
  src/nyx/imgui_draw_data_store.cc
  src/nyx/isolate_data.cc
  src/nyx/module_wrap.cc
  src/nyx/nyx.cc
  src/nyx/nyx_binding.cc
  src/nyx/nyx_fs.cc
  src/nyx/nyx_imgui.cc
  src/nyx/nyx_memory.cc
  src/nyx/process_binding.cc
  src/nyx/realm.cc
  src/nyx/timers.cc
  src/nyx/util.cc)

add_library(nyx STATIC ${NYX_SOURCES})
add_library(nyx::nyx ALIAS nyx)

target_js_sources(nyx lib ${CMAKE_CURRENT_BINARY_DIR}/compiled_builtins.cc)
target_include_directories(nyx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nyx PUBLIC uv_a simdutf v8 imgui)
target_compile_definitions(nyx PUBLIC
  NYX_DEBUG WIN32_LEAN_AND_MEAN
  NYX_IMGUI_V8_INTEGRATION)

install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/typings
  DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
  FILES_MATCHING PATTERN "*.d.ts")
