cmake_minimum_required(VERSION 3.24)
project(screenfleet_host LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C++ runtime: the .exe runs without installing the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/permissive->
                    $<$<COMPILE_LANGUAGE:CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/MP> $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)

set(PROTOCOL_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/include)
set(DRIVER_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../driver/include)

# Capture, encode, streaming engine and settings: shared by both executables.
add_library(sstcore STATIC
    src/log.cpp
    src/outputs.cpp
    src/capture.cpp
    src/converter.cpp
    src/encoder.cpp
    src/net.cpp
    src/session.cpp
    src/adb.cpp
    src/virtual_display.cpp
    src/engine.cpp
    src/settings.cpp
    src/driver_info.cpp
    src/crypto.cpp
    src/channel.cpp
    src/touch_input.cpp
)
target_include_directories(sstcore PUBLIC src ${PROTOCOL_INCLUDE} ${DRIVER_INCLUDE})
target_link_libraries(sstcore PUBLIC d3d11 dxgi mfplat mfuuid ws2_32 iphlpapi bcrypt crypt32 winmm cfgmgr32 shell32 ole32)

# The tray app.
add_executable(ScreenFleet WIN32
    src/app/app_main.cpp
    src/app/main_window.cpp
    src/app/canvas.cpp
    src/app/theme.cpp
    src/app/tray_icon.cpp
    res/app.rc
)
target_link_libraries(ScreenFleet PRIVATE sstcore d2d1 dwrite dwmapi uxtheme comctl32)
# res/app.manifest (DPI awareness, common controls 6) replaces the linker's default.
target_link_options(ScreenFleet PRIVATE /MANIFEST:NO)

# Command-line host, for testing.
add_executable(sst-host src/main.cpp)
target_link_libraries(sst-host PRIVATE sstcore)

# Stand-in for the Android app: lets the host be tested end-to-end on one PC.
add_executable(sst-fake-device
    tools/fake_device.cpp
    src/log.cpp
    src/net.cpp
)
target_include_directories(sst-fake-device PRIVATE src ${PROTOCOL_INCLUDE})
target_link_libraries(sst-fake-device PRIVATE ws2_32)
