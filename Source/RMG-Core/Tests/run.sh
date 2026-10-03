#!/bin/sh
# Host-side tests for the practice client: no emulator, Qt, or core library
# needed - just a C++20 compiler (c++, or set CXX). Run from anywhere:
#   Source/RMG-Core/Tests/run.sh
# Paths containing spaces are not supported.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CORE="$HERE/.."
OUT="${TMPDIR:-/tmp}/rmgk-practice-tests"
mkdir -p "$OUT"

# Core sources under test.
SOURCES="ReplayMemory.cpp ReplayEventBuilder.cpp"
# Sources that make m64p::Core (the DebugMem* function pointers) exist.
SUPPORT="m64p/Api.cpp m64p/CoreApi.cpp m64p/ConfigApi.cpp Library.cpp"

LIBS=""
case "$(uname -s)" in
    MINGW*|MSYS*) LIBS="-lws2_32" ;;
    Linux)        LIBS="-ldl" ;;
esac

FILES=""
for f in $SOURCES $SUPPORT; do
    FILES="$FILES $CORE/$f"
done

${CXX:-c++} -std=c++20 -Wall -Wextra -pthread -I"$CORE" -I"$HERE" -DRMGK_GAME_STATS \
    "$HERE"/*.cpp $FILES $LIBS -o "$OUT/tests"
"$OUT/tests"
