cmake_minimum_required(VERSION 3.24)
project(memclean VERSION 2.0.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "memclean requires Windows x64.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# All runtime dependencies come from Windows. MSVC uses the static CRT (/MT).
function(memclean_compile_options target)
    target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        _WIN32_WINNT=0x0A00 WINVER=0x0A00
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive- /sdl /guard:cf)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -finput-charset=UTF-8)
    endif()
endfunction()

add_library(memclean_core STATIC src/core.cpp src/core.h)
target_include_directories(memclean_core PUBLIC src)
target_compile_definitions(memclean_core PUBLIC PSAPI_VERSION=2)
target_link_libraries(memclean_core PUBLIC advapi32 psapi user32)
memclean_compile_options(memclean_core)

add_library(memclean_tasks STATIC
    src/task_manager.h
    src/task_sampling.cpp
    src/task_actions.cpp
    src/task_system.cpp
    src/task_startup.cpp
)
target_link_libraries(memclean_tasks PUBLIC memclean_core pdh iphlpapi wtsapi32 shell32 ole32)
memclean_compile_options(memclean_tasks)

add_executable(memclean WIN32 src/main.cpp src/task_window.cpp src/task_dialogs.cpp src/task_dialogs.h resources/app.rc)
target_include_directories(memclean PRIVATE resources)
target_link_libraries(memclean PRIVATE memclean_tasks comctl32 shell32 gdi32 comdlg32)
memclean_compile_options(memclean)
if(MSVC)
    # The resource file embeds the asInvoker manifest and Common Controls v6.
    target_link_options(memclean PRIVATE /MANIFEST:NO /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /guard:cf)
else()
    target_link_options(memclean PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(memclean_tests tests/core_tests.cpp)
    target_link_libraries(memclean_tests PRIVATE memclean_core)
    memclean_compile_options(memclean_tests)

    add_executable(memclean_fixture WIN32 tests/fixture.cpp)
    target_link_libraries(memclean_fixture PRIVATE shell32)
    memclean_compile_options(memclean_fixture)
    add_custom_command(TARGET memclean_fixture POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:memclean_fixture>
            $<TARGET_FILE_DIR:memclean_fixture>/memclean_foreground_fixture.exe
        VERBATIM
    )
    add_dependencies(memclean_tests memclean_fixture)

    if(MSVC)
        target_link_options(memclean_tests PRIVATE /guard:cf)
        target_link_options(memclean_fixture PRIVATE /guard:cf)
    else()
        target_link_options(memclean_tests PRIVATE -municode -static)
        target_link_options(memclean_fixture PRIVATE -municode -static)
    endif()

    add_test(NAME safety_and_isolated_working_set COMMAND memclean_tests
        $<TARGET_FILE:memclean_fixture>
        $<TARGET_FILE_DIR:memclean_fixture>/memclean_foreground_fixture.exe
    )
    set_tests_properties(safety_and_isolated_working_set PROPERTIES TIMEOUT 60)

    add_executable(memclean_task_tests tests/task_tests.cpp)
    target_link_libraries(memclean_task_tests PRIVATE memclean_tasks)
    memclean_compile_options(memclean_task_tests)
    add_dependencies(memclean_task_tests memclean_fixture)
    if(MSVC)
        target_link_options(memclean_task_tests PRIVATE /guard:cf)
    else()
        target_link_options(memclean_task_tests PRIVATE -municode -static)
    endif()
    add_test(NAME task_manager_safety_and_actions COMMAND memclean_task_tests $<TARGET_FILE:memclean_fixture>)
    set_tests_properties(task_manager_safety_and_actions PROPERTIES TIMEOUT 90)
endif()
