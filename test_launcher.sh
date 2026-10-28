#!/usr/bin/env bash
set -euo pipefail
export PATH=/mingw64/bin:/usr/bin:$PATH
cd "$(dirname "$0")"
g++ -std=c++17 -O2 -Wall -Wextra -Werror -municode -static -o launcher_tests.exe launcher_tests.cpp -lkernel32
./launcher_tests.exe
