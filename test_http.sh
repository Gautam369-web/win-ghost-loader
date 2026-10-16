#!/usr/bin/env bash
set -euo pipefail
export PATH=/mingw64/bin:/usr/bin:$PATH
cd "$(dirname "$0")"
g++ -std=c++17 -O2 -static -o http_smoke.exe http_smoke.cpp -lwininet
./http_smoke.exe "$@"
