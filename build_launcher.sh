#!/usr/bin/env bash
set -euo pipefail
export PATH=/mingw64/bin:/usr/bin:$PATH
cd "$(dirname "$0")"

# Compile the UAC manifest resource (requireAdministrator)
windres NexoraX.rc -o NexoraX_res.o

# Build launcher with embedded manifest so Windows auto-elevates it
g++ -std=c++17 -O2 -s -Wall -Wextra -municode -mwindows -static \
    -o NexoraX.exe VedaLauncher.cpp NexoraX_res.o \
    -luser32 -lkernel32 -lpsapi

# Clean up temp resource object
rm -f NexoraX_res.o

# UPX pack the launcher too
upx --best NexoraX.exe

echo LAUNCHER_BUILD_OK
