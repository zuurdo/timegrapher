cmake_minimum_required(VERSION 3.2)


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES /usr/local/lib ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES})
set(CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES /usr/local/Cellar ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES})

#add_subdirectory(lib/portaudio)
link_directories(/usr/local/lib)
add_library(portaudio /usr/local/lib/libportaudio.a)
set_target_properties(portaudio PROPERTIES LINKER_LANGUAGE CXX)

#add_executable(timegrapher main.cpp)
add_executable(timegrapher lib/portaudio/examples/paex_record.c)

target_link_libraries(timegrapher libportaudio.a)

project(timegrapher)