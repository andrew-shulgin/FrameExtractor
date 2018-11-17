cmake_minimum_required(VERSION 2.8)
set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules)
project(frame_extractor C)

set(CMAKE_C_STANDARD 99)

add_executable(frame_extractor main.c jsmn.c jsmn.h json.h json.c)

find_package(FFmpeg REQUIRED)
if (FFMPEG_FOUND)
    include_directories(${FFMPEG_INCLUDE_DIR})
    target_link_libraries(frame_extractor ${FFMPEG_LIBAVCODEC})
    target_link_libraries(frame_extractor ${FFMPEG_LIBAVFILTER})
    target_link_libraries(frame_extractor ${FFMPEG_LIBAVFORMAT})
    target_link_libraries(frame_extractor ${FFMPEG_LIBAVUTIL})
else (FFMPEG_FOUND)
    message(FATAL_ERROR "FFmpeg libraries not found!")
endif (FFMPEG_FOUND)

target_link_libraries(frame_extractor "-lm")
target_link_libraries(frame_extractor "-lpthread")
