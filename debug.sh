#!/usr/bin/env bash

# run from script dir so relative paths work
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pushd "$SCRIPT_DIR" > /dev/null || exit 1

TILESET_FILE="${CGSME_TILESET:-tileset.json}"
SOFT_MODE=0
FORWARD_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --soft|-s)
            SOFT_MODE=1
            if [ $# -gt 1 ] && [[ "$2" == *.json ]] && [ -f "$2" ]; then
                TILESET_FILE="$2"
                shift
            fi
            shift
            ;;
        --soft=*)
            SOFT_MODE=1
            TILESET_FILE="${1#--soft=}"
            shift
            ;;
        *.json)
            if [ -f "$1" ]; then
                TILESET_FILE="$1"
            else
                FORWARD_ARGS+=("$1")
            fi
            shift
            ;;
        *)
            FORWARD_ARGS+=("$1")
            shift
            ;;
    esac
done

compile_debug_gen() {
    echo "---------------------------------------------------"
    echo "1. Compiling C Code (Baked + Soft Mode)..."
    echo "---------------------------------------------------"
    gcc -std=c11 -O3 -g -Dcgsme_DEBUG -D_GNU_SOURCE \
        main.c \
        cgsme_ruleset.c \
        cgsme_soft.c \
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
}

if [ "$SOFT_MODE" -eq 1 ]; then
    echo "---------------------------------------------------"
    echo "0. Soft Mode Enabled (${TILESET_FILE}) - Skipping Codegen"
    echo "---------------------------------------------------"
    if [ ! -f "cgsme_tileset_baked.h" ]; then
        gcc -std=c11 -O2 cgsme_codegen.c cgsme_ruleset.c -o cgsme_codegen.exe \
            && ./cgsme_codegen.exe "tileset.json" cgsme_tileset_baked.h
    fi
    NEEDS_COMPILE=0
    if [ ! -x "./debug_gen.exe" ]; then
        NEEDS_COMPILE=1
    else
        for src in *.c *.h tinycthread/*.c tinycthread/*.h; do
            if [ -f "$src" ] && [ "$src" -nt "./debug_gen.exe" ]; then
                NEEDS_COMPILE=1
                break
            fi
        done
    fi
    if [ "$NEEDS_COMPILE" -eq 1 ]; then
        compile_debug_gen
    fi
    echo "---------------------------------------------------"
    echo "2. Running Generator (--soft ${TILESET_FILE})..."
    echo "---------------------------------------------------"
    ./debug_gen.exe --cgsme-debug --soft "$TILESET_FILE" "${FORWARD_ARGS[@]}"
else
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
    compile_debug_gen
    echo "---------------------------------------------------"
    echo "2. Running Generator (Baked)..."
    echo "---------------------------------------------------"
    ./debug_gen.exe --cgsme-debug "${FORWARD_ARGS[@]}"
fi

echo ""
if [ -f "render_maze_png.py" ]; then
    echo "---------------------------------------------------"
    echo "2b. Rendering Map/Texture PNG (maze_texture.png)..."
    echo "---------------------------------------------------"
    python3 render_maze_png.py -i maze.txt -t "$TILESET_FILE" -o maze_texture.png
    echo ""
fi
echo "---------------------------------------------------"
echo "3. Launching Visualizer..."
echo "---------------------------------------------------"

CGSME_TILESET="$TILESET_FILE" love .

echo "Done."

popd > /dev/null || true
