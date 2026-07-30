cmake_minimum_required(VERSION 3.20)

project(AudioControl LANGUAGES CXX)

add_executable(audioctl AudioControl.cpp AudioControl.rc)
target_compile_features(audioctl PRIVATE cxx_std_17)
target_compile_definitions(audioctl PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(audioctl PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(audioctl PRIVATE Ole32 Propsys)

if(MSVC)
  set_property(TARGET audioctl PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
