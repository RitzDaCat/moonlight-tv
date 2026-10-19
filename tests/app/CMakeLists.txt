add_subdirectory(e2e)

add_unit_test(test_settings test_settings.c)

add_subdirectory(backend)
add_subdirectory(ui)
add_unit_test(test_stream_input test_stream_input.c)
add_unit_test(test_stream_audio test_stream_audio.c)
add_unit_test(test_session_stats test_session_stats.c)
set_tests_properties(test_stream_input test_stream_audio test_session_stats PROPERTIES TIMEOUT 30)
add_executable(test_event_wait test_event_wait.c)
target_include_directories(test_event_wait PRIVATE ${CMAKE_SOURCE_DIR}/src/app ${SDL2_INCLUDE_DIRS})
target_link_libraries(test_event_wait PRIVATE ${SDL2_LIBRARIES})
add_test(NAME test_event_wait COMMAND test_event_wait)
set_tests_properties(test_event_wait PROPERTIES TIMEOUT 10)
add_executable(test_audio_control test_audio_control.c ${CMAKE_SOURCE_DIR}/src/app/stream/audio/audio_control.c)
target_include_directories(test_audio_control PRIVATE ${CMAKE_SOURCE_DIR}/src/app ${SDL2_INCLUDE_DIRS})
target_link_libraries(test_audio_control PRIVATE ${SDL2_LIBRARIES} m)
add_test(NAME test_audio_control COMMAND test_audio_control)
set_tests_properties(test_audio_control PROPERTIES TIMEOUT 10)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT TARGET_WEBOS AND NOT TARGET_STEAMLINK)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(test_tv_navigation test_tv_navigation.c)
    target_link_libraries(test_tv_navigation PRIVATE moonlight-lib)
    foreach(symbol apploader_create apploader_load apploader_cancel apploader_destroy apploader_state
            pcmanager_auto_discovery_start pcmanager_auto_discovery_stop pcmanager_request_update
            app_is_decoder_valid app_input_init_gamepad_mapping app_input_deinit_gamepad_mapping
            app_gs_client_new gs_download_cover SDL_CreateRenderer app_session_begin)
        target_link_options(test_tv_navigation PRIVATE "LINKER:--wrap=${symbol}")
    endforeach()
    add_test(NAME test_tv_navigation COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_tv_navigation.py $<TARGET_FILE:test_tv_navigation>)
    set_tests_properties(test_tv_navigation PROPERTIES TIMEOUT 30)
endif()
