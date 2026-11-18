cmake_minimum_required(VERSION 3.20)
project(hvcam LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

if(WIN32)
    # Static runtime: hvcam-host.exe ships with no DLL dependencies.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    add_executable(hvcam-host
        windows/main.cpp
        windows/mf_capture.cpp
        windows/vsock_server.cpp
        windows/frame_slots.cpp
    )
    target_link_libraries(hvcam-host PRIVATE
        mfplat mfreadwrite mf mfuuid ole32 ws2_32
    )
    target_compile_definitions(hvcam-host PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    )
else()
    find_package(Threads REQUIRED)

    add_library(hvcam SHARED linux/hvcam_client.c)
    target_include_directories(hvcam PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/linux
    )
    set_target_properties(hvcam PROPERTIES
        PUBLIC_HEADER "include/protocol.h;linux/hvcam_client.h"
    )

    add_executable(hvcam-dump linux/hvcam_dump.c)
    target_link_libraries(hvcam-dump PRIVATE hvcam)

    enable_testing()
    add_executable(test_loopback tests/test_loopback.c)
    target_link_libraries(test_loopback PRIVATE hvcam Threads::Threads)
    add_test(NAME loopback COMMAND test_loopback)

    add_executable(test_score tests/test_score.c)
    add_test(NAME score COMMAND test_score)

    find_program(PYTHON3_EXECUTABLE NAMES python3 python)
    if(PYTHON3_EXECUTABLE)
        add_test(NAME python_ring
                 COMMAND ${PYTHON3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ring.py)
        # exit 77 = numpy missing → skipped, not failed.
        # HVCAM_LIB pins the .so this build just produced, so the
        # test works with any -B dir and can't pick up a stale
        # default-build library.
        set_tests_properties(python_ring PROPERTIES
            SKIP_RETURN_CODE 77
            ENVIRONMENT "HVCAM_LIB=$<TARGET_FILE:hvcam>")
    endif()
endif()
