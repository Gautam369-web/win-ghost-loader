#!/usr/bin/env bash
set -euo pipefail
export PATH=/mingw64/bin:/usr/bin:$PATH
cd /c/Users/Rossie/Downloads/ScreenCapture
g++ -std=c++17 -O2 -shared -o ScreenCapture.review.dll ScreenCaptureDLL.cpp \
  -lgdi32 -lgdiplus -ldwmapi -ld3d11 -ldxgi -lole32 -loleaut32 -luuid \
  -luser32 -lkernel32 -lpsapi -lwininet -lcomctl32 \
  -static-libgcc -static-libstdc++ -Wl,-Bstatic -lpthread -Wl,-Bdynamic
echo BUILD_OK
