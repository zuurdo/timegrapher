cmake_minimum_required(VERSION 3.16)
project(timegrapher CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_OSX_SYSROOT "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk")

include(FetchContent)

# FTXUI (terminal UI)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG v5.0.0)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ftxui)

# PortAudio: use the system one (macOS: brew install portaudio, Linux: libportaudio-dev)
find_path(PORTAUDIO_INCLUDE_DIR portaudio.h
  HINTS /opt/homebrew/include /usr/local/include)
find_library(PORTAUDIO_LIBRARY portaudio
  HINTS /opt/homebrew/lib /usr/local/lib)
if(NOT PORTAUDIO_INCLUDE_DIR OR NOT PORTAUDIO_LIBRARY)
  message(FATAL_ERROR "PortAudio not found (brew install portaudio)")
endif()

add_executable(timegrapher main.cpp)
target_include_directories(timegrapher PRIVATE ${PORTAUDIO_INCLUDE_DIR})
target_link_libraries(timegrapher PRIVATE ${PORTAUDIO_LIBRARY}
  ftxui::component ftxui::dom ftxui::screen)

# Offline test of the DSP core on synthetic watch audio (no audio hardware needed)
add_executable(test_synth test_synth.cpp)
