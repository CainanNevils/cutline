cmake_minimum_required(VERSION 3.28)
project(cutline VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Cutline is Windows/MSVC only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- dependencies (installed by scripts/setup.ps1 into deps/) ---------------------------------
set(CUTLINE_DEPS ${CMAKE_SOURCE_DIR}/deps CACHE PATH "Dependency root")
set(CUTLINE_QT_DIR ${CUTLINE_DEPS}/qt/6.8.3/msvc2022_64 CACHE PATH "Qt kit")
list(APPEND CMAKE_PREFIX_PATH ${CUTLINE_QT_DIR})
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg Multimedia)
set(CMAKE_AUTOMOC ON)

set(FFMPEG_DIR ${CUTLINE_DEPS}/ffmpeg)
add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg SYSTEM INTERFACE ${FFMPEG_DIR}/include)
foreach(lib avformat avcodec avfilter avutil swresample swscale)
    target_link_libraries(ffmpeg INTERFACE ${FFMPEG_DIR}/lib/${lib}.lib)
endforeach()
# runtime DLLs next to every exe
file(GLOB FFMPEG_DLLS ${FFMPEG_DIR}/bin/*.dll)
file(COPY ${FFMPEG_DLLS} DESTINATION ${CMAKE_BINARY_DIR}/bin)
file(COPY ${FFMPEG_DIR}/bin/ffmpeg.exe ${FFMPEG_DIR}/bin/ffprobe.exe DESTINATION ${CMAKE_BINARY_DIR}/bin)

add_library(thirdparty INTERFACE)
target_include_directories(thirdparty SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

# --- compile options -------------------------------------------------------------------------------
add_compile_options(
    /W4 /permissive- /utf-8 /Zc:__cplusplus /MP
    /external:anglebrackets /external:W0
    /wd4251   # dll-interface warnings from Qt headers in some configs
)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    _CRT_SECURE_NO_WARNINGS)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(CUTLINE_DEBUG=1)
endif()

# --- HLSL shaders compiled at build time to embedded bytecode headers -------------------------
find_program(FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" REQUIRED)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(CUTLINE_SHADER_HEADERS "")
function(cutline_shader file entry profile var)
    set(src ${CMAKE_SOURCE_DIR}/src/render/shaders/${file})
    set(out ${SHADER_OUT}/${var}.h)
    add_custom_command(OUTPUT ${out}
        COMMAND ${FXC} /nologo /O3 /T ${profile} /E ${entry} /Vn ${var} /Fh ${out} ${src}
        DEPENDS ${src} ${CMAKE_SOURCE_DIR}/src/render/shaders/common.hlsli
        COMMENT "fxc ${file}:${entry}" VERBATIM)
    set(CUTLINE_SHADER_HEADERS ${CUTLINE_SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()

include_directories(${CMAKE_SOURCE_DIR}/src ${SHADER_OUT})

enable_testing()
add_subdirectory(src)
add_subdirectory(tools)
add_subdirectory(tests)
