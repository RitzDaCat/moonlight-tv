# Builds a second, independent copy of moonlight-common-c for split screen streaming.
#
# moonlight-common-c keeps its connection state in globals, as do the session callbacks that feed
# it (video, audio, connection listener). Both are merged into one relocatable object and every
# global symbol it defines gets an mlc2_ prefix, so the copy links next to the normal one without
# clashing. File-static state is already private to each copy. src/app/stream/mlc_slot.c binds
# the prefixed entry points.

function(add_mlc_second_copy target)
    set(glue_target mlc2-glue)
    add_library(${glue_target} OBJECT
            ${CMAKE_SOURCE_DIR}/src/app/stream/video/session_video.c
            ${CMAKE_SOURCE_DIR}/src/app/stream/audio/session_audio.c
            ${CMAKE_SOURCE_DIR}/src/app/stream/connection/session_connnection.c)
    # Same flags as the slot 0 copy compiled into ${target}, without a target dependency on it
    set_target_properties(${glue_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED TRUE)
    target_include_directories(${glue_target} PRIVATE $<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>)
    target_compile_definitions(${glue_target} PRIVATE $<TARGET_PROPERTY:${target},COMPILE_DEFINITIONS>)
    target_compile_options(${glue_target} PRIVATE $<TARGET_PROPERTY:${target},COMPILE_OPTIONS>)
    target_link_libraries(${glue_target} PRIVATE $<TARGET_PROPERTY:${target},LINK_LIBRARIES>)
    add_dependencies(${glue_target} moonlight-common-c)

    set(merged ${CMAKE_CURRENT_BINARY_DIR}/mlc2-merged.o)
    set(output ${CMAKE_CURRENT_BINARY_DIR}/mlc2.o)
    add_custom_command(OUTPUT ${output}
            COMMAND ${CMAKE_LINKER} -r -o ${merged} $<TARGET_OBJECTS:${glue_target}>
            --whole-archive $<TARGET_FILE:moonlight-common-c> $<TARGET_FILE:enet> --no-whole-archive
            COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DOBJCOPY=${CMAKE_OBJCOPY} -DINPUT=${merged}
            -DOUTPUT=${output} -DPREFIX=mlc2_ -P ${CMAKE_SOURCE_DIR}/cmake/MlcRename.cmake
            DEPENDS ${glue_target} moonlight-common-c enet $<TARGET_OBJECTS:${glue_target}>
            ${CMAKE_SOURCE_DIR}/cmake/MlcRename.cmake
            COMMAND_EXPAND_LISTS
            VERBATIM)
    add_custom_target(mlc2-object DEPENDS ${output})
    add_dependencies(${target} mlc2-object)
    target_sources(${target} PRIVATE ${output})
    set_source_files_properties(${output} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
endfunction()
