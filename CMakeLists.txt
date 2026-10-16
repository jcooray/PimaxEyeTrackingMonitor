cmake_minimum_required(VERSION 3.15)
project(PimaxEyeTrackingMonitor VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C++ runtime, so the exe runs without the Visual C++ Redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(PimaxEyeTrackingMonitor WIN32 src/PimaxEyeTrackingMonitor.cpp src/PimaxEyeTrackingMonitor.rc)
target_include_directories(PimaxEyeTrackingMonitor PRIVATE third_party/openvr)
target_compile_definitions(PimaxEyeTrackingMonitor PRIVATE UNICODE _UNICODE)
target_link_libraries(PimaxEyeTrackingMonitor PRIVATE user32 gdi32 ole32)
