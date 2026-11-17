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
endif()
