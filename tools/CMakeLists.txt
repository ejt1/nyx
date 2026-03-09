add_executable(js2c js2c.cc)
target_link_libraries(js2c PRIVATE uv_a)

# Function for nyx internal builtins
function(target_js_sources TARGET DIRECTORY OUTPUT_CC)
  file(GLOB_RECURSE JS_FILES "${DIRECTORY}/*.js")

  add_custom_command(
    OUTPUT ${OUTPUT_CC}
    COMMAND $<TARGET_FILE:js2c>
    ARGS --root ${CMAKE_CURRENT_SOURCE_DIR} ${OUTPUT_CC} ${DIRECTORY}
    DEPENDS js2c ${JS_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating ${OUTPUT_CC} from JavaScript files..."
    VERBATIM
  )

  target_sources(${TARGET} PRIVATE ${OUTPUT_CC})
  set_source_files_properties(${OUTPUT_CC} PROPERTIES GENERATED TRUE)
endfunction()

# Function for external builtins (used by libraries that extend nyx)
# Generates a .cc and .h file with a RegisterBuiltins() function
function(target_external_js_sources TARGET DIRECTORY OUTPUT_CC NAMESPACE)
  file(GLOB_RECURSE JS_FILES "${DIRECTORY}/*.js")

  # Header file path (replace .cc with .h)
  string(REGEX REPLACE "\\.cc$" ".h" OUTPUT_H "${OUTPUT_CC}")

  add_custom_command(
    OUTPUT ${OUTPUT_CC} ${OUTPUT_H}
    COMMAND $<TARGET_FILE:js2c>
    ARGS --root ${CMAKE_CURRENT_SOURCE_DIR} --external --namespace ${NAMESPACE} ${OUTPUT_CC} ${DIRECTORY}
    DEPENDS js2c ${JS_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating ${OUTPUT_CC} and ${OUTPUT_H} from JavaScript files..."
    VERBATIM
  )

  target_sources(${TARGET} PRIVATE ${OUTPUT_CC})
  set_source_files_properties(${OUTPUT_CC} PROPERTIES GENERATED TRUE)
  set_source_files_properties(${OUTPUT_H} PROPERTIES GENERATED TRUE)
endfunction()
