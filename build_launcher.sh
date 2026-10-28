#!/usr/bin/env bash
set -euo pipefail
export PATH=/mingw64/bin:/usr/bin:$PATH
cd "$(dirname "$0")"
g++ -std=c++17 -O2 -s -Wall -Wextra -municode -mwindows -static -o NexoraX.exe VedaLauncher.cpp -luser32 -lkernel32
echo LAUNCHER_BUILD_OK
