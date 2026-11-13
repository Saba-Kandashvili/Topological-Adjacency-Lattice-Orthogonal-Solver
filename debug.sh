#!/usr/bin/env bash

# Ensure script runs from its own directory so relative paths resolve
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pushd "$SCRIPT_DIR" > /dev/null || exit 1

TILESET_FILE="${CGSME_TILESET:-tileset.json}"
if [ $# -gt 0 ] && [[ "$1" == *.json ]] && [ -f "$1" ]; then
    TILESET_FILE="$1"
    shift
fi

echo "---------------------------------------------------"
echo "0. Baking ${TILESET_FILE} -> cgsme_tileset_baked.h..."
echo "---------------------------------------------------"

gcc -std=c11 -O2 cgsme_codegen.c cgsme_ruleset.c -o cgsme_codegen.exe \
    && ./cgsme_codegen.exe "$TILESET_FILE" cgsme_tileset_baked.h

if [ $? -ne 0 ]; then
    echo "Tileset baking failed!"
    read -r -p "Press Enter to exit..."
    popd > /dev/null || true
    exit 1
fi
echo ""

echo "---------------------------------------------------"
echo "1. Compiling C Code..."
echo "---------------------------------------------------"

# Build with cgsme_DEBUG and include the debug source so instrumentation is available
gcc -std=c11 -O3 -g -Dcgsme_DEBUG -D_GNU_SOURCE \
    main.c \
    generator.c \
    cgsme_debug.c \
    cgsme_utils.c \
    cgsme_noise.c \
    cgsme_topology.c \
    cgsme_solver.c \
    tinycthread/tinycthread.c \
    -o debug_gen.exe -I. -pthread -lm

if [ $? -ne 0 ]; then
    echo "Compilation Failed!"
    read -r -p "Press Enter to exit..."
    popd > /dev/null || true
    exit 1
fi

echo "Compilation Successful."
echo ""
echo "---------------------------------------------------"
echo "2. Running Generator..."
echo "---------------------------------------------------"

./debug_gen.exe --cgsme-debug "$@"

echo ""
echo "---------------------------------------------------"
echo "3. Launching Visualizer..."
echo "---------------------------------------------------"

CGSME_TILESET="$TILESET_FILE" love .

echo "Done."

popd > /dev/null || true
