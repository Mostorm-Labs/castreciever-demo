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
    src/Log.cpp
    src/App.cpp
    src/MainWindow.cpp
    src/core/ClockMapper.cpp
    src/core/FrameQueue.cpp
    src/core/FlowController.cpp
    src/core/MediaCore.cpp
    src/source/SessionManager.cpp
    src/source/UsbUvcSourceAdapter.cpp
    src/airplay/AirPlayFeatures.cpp
    src/airplay/AirPlayDeviceId.cpp
    src/airplay/AirPlayDiscoveryService.cpp
    src/airplay/AirPlaySourceAdapter.cpp
    src/hid/AxtpHidMediaProtocol.cpp
    src/hid/AxtpRuntimeHidAdapter.cpp
    src/hid/HidMediaExperimentalAdapter.cpp
    src/device/UvcDeviceEnumerator.cpp
    src/device/UacDeviceEnumerator.cpp
    src/video/MfCapturePreviewPlayer.cpp
    src/video/SourceReaderD3D11Player.cpp
    src/video/SelfTestVideoPlayer.cpp
    src/audio/WasapiPcmRelay.cpp
    src/ui/OverlayControls.cpp
)

target_compile_features(UsbCastReceiver PRIVATE cxx_std_17)

set(AXTP_CPP_RUNTIME_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/axtp-cpp-runtime"
    CACHE PATH "Path to the AXTP C++ runtime repository")

if(NOT EXISTS "${AXTP_CPP_RUNTIME_ROOT}/core/CMakeLists.txt")
    message(FATAL_ERROR
        "AXTP C++ runtime was not found. "
        "Set AXTP_CPP_RUNTIME_ROOT to the axtp-cpp-runtime repository directory.")
endif()

set(AXTP_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AXTP_BUILD_JSON_RPC OFF CACHE BOOL "" FORCE)
set(AXTP_BUILD_OPTIONAL_TRANSPORTS ON CACHE BOOL "" FORCE)
add_subdirectory(
    "${AXTP_CPP_RUNTIME_ROOT}/core"
    "${CMAKE_CURRENT_BINARY_DIR}/axtp-cpp-runtime-core"
    EXCLUDE_FROM_ALL)

if(NOT TARGET axtp_transport_hidapi)
    message(FATAL_ERROR "AXTP C++ runtime did not provide axtp_transport_hidapi.")
endif()

target_compile_definitions(UsbCastReceiver PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
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
    iphlpapi
    ws2_32
    bcrypt
    axtp_transport_hidapi
)
