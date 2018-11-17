#!/bin/sh
i686-w64-mingw32-gcc   -std=c99 main.c json.c jsmn.c -I"$HOME/ffmpeg-win/ffmpeg-20180227-fa0c9d6-win32-dev/include" -lm -lpthread -L"$HOME/ffmpeg-win/ffmpeg-20180227-fa0c9d6-win32-shared/bin" -lavcodec-58 -lavfilter-7 -lavformat-58 -lavutil-56 -o FrameExtractor32.exe
x86_64-w64-mingw32-gcc -std=c99 main.c json.c jsmn.c -I"$HOME/ffmpeg-win/ffmpeg-20180227-fa0c9d6-win64-dev/include" -lm -lpthread -L"$HOME/ffmpeg-win/ffmpeg-20180227-fa0c9d6-win64-shared/bin" -lavcodec-58 -lavfilter-7 -lavformat-58 -lavutil-56 -o FrameExtractor64.exe
