cmake_minimum_required(VERSION 3.15)
project(pendulum_cursor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(PENDULUM_BUILD_TESTS "Build the portable physics/render validation harness" OFF)

# --- The cursor application (Windows only) -----------------------------------
if(WIN32)
    enable_language(RC)   # compile resources/app.rc (icon + version metadata)

    # WIN32 => GUI subsystem: launches with no console window.
    add_executable(pendulum_cursor WIN32
        src/main.cpp
        src/Physics.cpp
        src/SnapMode.cpp
        src/Renderer.cpp
        src/CursorController.cpp
        src/ConfigFile.cpp
        src/CursorPoses.cpp
        src/AppWindow.cpp
        src/BeatDsp.cpp
        src/BeatWorker.cpp
        src/LoopbackCapture.cpp
        src/GifDecoder.cpp
        src/Dancer.cpp
        resources/app.rc
    )
    # 'include' for headers, 'resources' so the .rc finds app.ico.
    target_include_directories(pendulum_cursor PRIVATE include resources)
    target_link_libraries(pendulum_cursor PRIVATE user32 gdi32 winmm shell32 ole32)
    if(MSVC)
        target_compile_options(pendulum_cursor PRIVATE /O2 /W4 /permissive-)
    else()
        target_compile_options(pendulum_cursor PRIVATE -O2 -Wall -Wextra)
    endif()

    # Copy the default config files next to the exe so they're ready to edit.
    foreach(cfgfile pendulum.conf cursors.conf)
        add_custom_command(TARGET pendulum_cursor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${CMAKE_CURRENT_SOURCE_DIR}/${cfgfile}
                    $<TARGET_FILE_DIR:pendulum_cursor>/${cfgfile})
    endforeach()
    # The dancing GIFs (.gifbpm) go in an actors/ folder next to the exe.
    add_custom_command(TARGET pendulum_cursor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/actors
                $<TARGET_FILE_DIR:pendulum_cursor>/actors)
elseif(NOT PENDULUM_BUILD_TESTS)
    message(FATAL_ERROR
        "The cursor app targets Windows. On other platforms, configure with "
        "-DPENDULUM_BUILD_TESTS=ON to build just the portable physics tests.")
endif()

# --- Portable validation harness (no Windows headers) ------------------------
if(PENDULUM_BUILD_TESTS)
    add_executable(physics_tests
        test/test_physics.cpp
        src/Physics.cpp
        src/SnapMode.cpp
        src/Renderer.cpp
        src/ConfigFile.cpp
        src/CursorPoses.cpp
        src/GifDecoder.cpp
    )
    target_include_directories(physics_tests PRIVATE include)

    # Beat detector + GIF dancer self-test (synthetic audio, no data files).
    add_executable(beat_tests
        test/test_beat.cpp
        src/BeatDsp.cpp
        src/BeatWorker.cpp
        src/GifDecoder.cpp
        src/Dancer.cpp
    )
    target_include_directories(beat_tests PRIVATE include)
    if(MSVC)
        target_compile_options(physics_tests PRIVATE /O2)
    else()
        target_compile_options(physics_tests PRIVATE -O2)
    endif()
endif()
