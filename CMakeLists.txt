cmake_minimum_required(VERSION 3.21)

project(UsbCastReceiver LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "UsbCastReceiver is a Windows-only PoC. Configure it on Windows 10/11 with Visual Studio 2022.")
endif()

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE USB_CAST_RECEIVER_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT USB_CAST_RECEIVER_GIT_SHA)
    set(USB_CAST_RECEIVER_GIT_SHA "unknown")
endif()

string(TIMESTAMP USB_CAST_RECEIVER_BUILD_TIME_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

add_executable(UsbCastReceiver WIN32
    src/main.cpp
    src/App.cpp
    src/MainWindow.cpp
    src/device/UvcDeviceEnumerator.cpp
    src/device/UacDeviceEnumerator.cpp
    src/video/MfCapturePreviewPlayer.cpp
    src/video/SourceReaderD3D11Player.cpp
    src/video/SelfTestVideoPlayer.cpp
    src/audio/WasapiPcmRelay.cpp
    src/ui/OverlayControls.cpp
)

target_compile_features(UsbCastReceiver PRIVATE cxx_std_17)

target_compile_definitions(UsbCastReceiver PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    USB_CAST_RECEIVER_GIT_SHA=L"${USB_CAST_RECEIVER_GIT_SHA}"
    USB_CAST_RECEIVER_BUILD_TIME_UTC=L"${USB_CAST_RECEIVER_BUILD_TIME_UTC}"
)

target_include_directories(UsbCastReceiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(UsbCastReceiver PRIVATE
    mfplat
    mf
    mfreadwrite
    mfuuid
    wmcodecdspuuid
    d3d11
    d3dcompiler
    dxgi
    user32
    gdi32
    ole32
    oleaut32
    uuid
    avrt
    mmdevapi
    propsys
    shell32
)
