cmake_minimum_required(VERSION 3.24)
project(sst_display LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Drivers must not depend on the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(WDK_ROOT "C:/Program Files (x86)/Windows Kits/10" CACHE PATH "Windows Kits root")
set(WDK_VERSION "10.0.26100.0" CACHE STRING "Windows Kits version")
set(UMDF_MINOR 25)
set(IDDCX_MINOR 4)

add_library(SstDisplay SHARED src/Driver.cpp)
target_compile_definitions(SstDisplay PRIVATE
    UMDF_VERSION_MAJOR=2 UMDF_VERSION_MINOR=${UMDF_MINOR}
    IDDCX_VERSION_MAJOR=1 IDDCX_VERSION_MINOR=${IDDCX_MINOR}
    UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(SstDisplay PRIVATE /W4 /permissive- /utf-8 /EHsc /guard:cf /Qspectre-)
target_link_options(SstDisplay PRIVATE /guard:cf)
target_include_directories(SstDisplay PRIVATE
    include
    ${WDK_ROOT}/Include/wdf/umdf/2.${UMDF_MINOR}
    ${WDK_ROOT}/Include/${WDK_VERSION}/um/iddcx/1.${IDDCX_MINOR})
target_link_libraries(SstDisplay PRIVATE
    ${WDK_ROOT}/Lib/wdf/umdf/x64/2.${UMDF_MINOR}/WdfDriverStubUm.lib
    ${WDK_ROOT}/Lib/${WDK_VERSION}/um/x64/iddcx/1.${IDDCX_MINOR}/IddCxStub.lib
    d3d11 dxgi avrt ntdll)
