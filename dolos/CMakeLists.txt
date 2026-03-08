add_subdirectory(vendor)

set(DOLOS_SOURCES
  src/dolos/dolos.cc
  src/dolos/dolos_binding.cc
  src/dolos/main_thread.cc
  src/dolos/backend/d3d12_imgui.cc
  src/dolos/backend/d3d12_renderer.cc
  src/dolos/backend/renderer.cc
  src/dolos/backend/win32_window.cc
  src/dolos/offset_cache.cc
  src/dolos/offset_registry.cc
  src/dolos/pattern_scanner.cc
  src/dolos/pe_builder.cc
  src/dolos/pipe_log.cc
)

add_library(dolos STATIC ${DOLOS_SOURCES})
add_library(nyx::dolos ALIAS dolos)

target_external_js_sources(dolos lib ${CMAKE_CURRENT_BINARY_DIR}/dolos_builtins.cc dolos_builtins)

target_include_directories(dolos PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(dolos PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(dolos PUBLIC nyx::nyx imgui lazy_importer minhook spdlog shlwapi Zydis)
target_compile_definitions(dolos PUBLIC SPDLOG_ACTIVE_LEVEL=1 WIN32_LEAN_AND_MEAN)

install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/typings
  DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
  FILES_MATCHING PATTERN "*.d.ts")
