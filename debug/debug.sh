#!/usr/bin/env bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CALLER_DIR="$(pwd)"

resolve_tileset() {
    local arg="$1"
    if [ -f "$arg" ]; then
        echo "$(cd "$(dirname "$arg")" && pwd)/$(basename "$arg")"
    elif [ -f "$CALLER_DIR/$arg" ]; then
        echo "$(cd "$(dirname "$CALLER_DIR/$arg")" && pwd)/$(basename "$arg")"
    elif [ -f "$ROOT_DIR/$arg" ]; then
        echo "$(cd "$(dirname "$ROOT_DIR/$arg")" && pwd)/$(basename "$arg")"
    else
        echo "$arg"
    fi
}

TILESET_FILE="${TALOS_TILESET:-$ROOT_DIR/tileset.json}"
SOFT_MODE=0
FORWARD_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h)
            echo "usage: ./debug/debug.sh [--soft path/to/tileset.json] [--size WxLxH] [-w N] [-l N] [-z N] [--seed N|random] [-f N]"
            exit 0
            ;;
        --soft|-s)
            SOFT_MODE=1
            if [ $# -gt 1 ] && [[ "$2" == *.json ]]; then
                TILESET_FILE="$(resolve_tileset "$2")"
                shift
            fi
            shift
            ;;
        --soft=*)
            SOFT_MODE=1
            TILESET_FILE="$(resolve_tileset "${1#--soft=}")"
            shift
            ;;
        *.json)
            if [ -f "$1" ] || [ -f "$CALLER_DIR/$1" ] || [ -f "$ROOT_DIR/$1" ]; then
                TILESET_FILE="$(resolve_tileset "$1")"
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

TILESET_FILE="$(resolve_tileset "$TILESET_FILE")"
pushd "$SCRIPT_DIR" > /dev/null || exit 1

compile_debug_gen() {
    echo "---------------------------------------------------"
    echo "1. Compiling C Code (Baked + Soft Mode)..."
    echo "---------------------------------------------------"
    gcc -std=c11 -O3 -g -DTALOS_DEBUG -D_GNU_SOURCE \
        "$SCRIPT_DIR/main.c" \
        "$ROOT_DIR/talos_ruleset.c" \
        "$ROOT_DIR/talos_soft.c" \
        "$ROOT_DIR/generator.c" \
        "$ROOT_DIR/talos_debug.c" \
        "$ROOT_DIR/talos_utils.c" \
        "$ROOT_DIR/talos_noise.c" \
        "$ROOT_DIR/talos_topology.c" \
        "$ROOT_DIR/talos_solver.c" \
        "$ROOT_DIR/tinycthread/tinycthread.c" \
        -o "$SCRIPT_DIR/debug_gen.exe" -I"$ROOT_DIR" -pthread -lm
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
    if [ ! -f "$ROOT_DIR/talos_tileset_baked.h" ]; then
        gcc -std=c11 -O2 "$ROOT_DIR/talos_codegen.c" "$ROOT_DIR/talos_ruleset.c" -I"$ROOT_DIR" -o "$SCRIPT_DIR/talos_codegen.exe" \
            && "$SCRIPT_DIR/talos_codegen.exe" "$ROOT_DIR/tileset.json" "$ROOT_DIR/talos_tileset_baked.h"
    fi
    NEEDS_COMPILE=0
    if [ ! -x "$SCRIPT_DIR/debug_gen.exe" ]; then
        NEEDS_COMPILE=1
    else
        for src in "$SCRIPT_DIR"/main.c "$ROOT_DIR"/*.c "$ROOT_DIR"/*.h "$ROOT_DIR"/tinycthread/*.c "$ROOT_DIR"/tinycthread/*.h; do
            if [ -f "$src" ] && [ "$src" -nt "$SCRIPT_DIR/debug_gen.exe" ]; then
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
    "$SCRIPT_DIR/debug_gen.exe" --talos-debug --soft "$TILESET_FILE" "${FORWARD_ARGS[@]}"
else
    echo "---------------------------------------------------"
    echo "0. Baking ${TILESET_FILE} -> talos_tileset_baked.h..."
    echo "---------------------------------------------------"
    gcc -std=c11 -O2 "$ROOT_DIR/talos_codegen.c" "$ROOT_DIR/talos_ruleset.c" -I"$ROOT_DIR" -o "$SCRIPT_DIR/talos_codegen.exe" \
        && "$SCRIPT_DIR/talos_codegen.exe" "$TILESET_FILE" "$ROOT_DIR/talos_tileset_baked.h"
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
    "$SCRIPT_DIR/debug_gen.exe" --talos-debug "${FORWARD_ARGS[@]}"
fi

echo ""
echo "---------------------------------------------------"
echo "3. Launching Visualizer..."
echo "---------------------------------------------------"

TALOS_TILESET="$TILESET_FILE" love "$SCRIPT_DIR"

echo "Done."

popd > /dev/null || true
