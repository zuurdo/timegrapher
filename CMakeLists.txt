cmake_minimum_required(VERSION 3.2)
project(timegrapher)

set(CMAKE_CXX_STANDARD 11)

set(PORTAUDIO_DIR "/Users/raulmartinpintos/Documents/repositories/timegrapher/lib/portaudio")
#set(GCC_COVERAGE_COMPILE_FLAGS "-lc++")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${GCC_COVERAGE_COMPILE_FLAGS}" )



find_package(PythonLibs REQUIRED)
find_package(Matplot++ QUIET)
if(NOT Matplot++_FOUND)
    # Put your FetchContent or CPM.cmake script here
    add_subdirectory(lib/matplotplusplus)
endif()

include_directories(${PYTHON_INCLUDE_DIRS})
include_directories(/opt/homebrew/lib/python3.11/site-packages/numpy/core/include)
include_directories(/opt/homebrew/Cellar/fftw/3.3.10_1/include)
include_directories(/opt/homebrew/Cellar/matplotplusplus/1.2.1/include)
include_directories(IMGUI_DIR)

find_library(COREAUDIO_FRAMEWORK CoreAudio)
find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox)
find_library(AUDIOUNIT_FRAMEWORK AudioUnit)
find_library(CORESERVICES_FRAMEWORK CoreServices)
find_library(CRBON_FRAMEWORK Carbon)
#find_library(MATPLOT_FRAMEWORK matplot)

add_executable(timegrapher
        include/matplotlibcpp.h
        main.cpp)
#add_executable(timegrapher lib/portaudio/examples/paex_read_write_wire.c)


target_link_libraries(timegrapher ${PYTHON_LIBRARIES})
target_link_libraries(timegrapher /Users/raulmartinpintos/Documents/repositories/timegrapher/lib/portaudio/lib/.libs/libportaudio.a)
target_link_libraries(timegrapher /opt/homebrew/Cellar/fftw/3.3.10_1/lib/libfftw3.a)
target_link_libraries(timegrapher ${COREAUDIO_FRAMEWORK})
target_link_libraries(timegrapher ${AUDIOTOOLBOX_FRAMEWORK})
target_link_libraries(timegrapher ${AUDITUNIT_FRAMEWORK})
target_link_libraries(timegrapher ${CORESERVICES_FRAMEWORK})
target_link_libraries(timegrapher ${CARBON_FRAMEWORK})
target_link_libraries(timegrapher Matplot++::matplot)

