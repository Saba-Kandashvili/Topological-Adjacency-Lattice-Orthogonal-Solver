@echo off
REM Ensure script runs from its own directory so relative paths resolve
pushd %~dp0

echo ---------------------------------------------------
echo 0. Baking tileset.json -^> cgsme_tileset_baked.h...
echo ---------------------------------------------------
gcc -std=c11 -O2 cgsme_codegen.c cgsme_ruleset.c -o cgsme_codegen.exe
if %ERRORLEVEL% NEQ 0 (
    echo Tileset codegen compile failed!
    pause
    exit /b
)
cgsme_codegen.exe tileset.json cgsme_tileset_baked.h
if %ERRORLEVEL% NEQ 0 (
    echo Tileset baking failed!
    pause
    exit /b
)
echo.


echo ---------------------------------------------------
echo 1. Compiling C Code...
echo ---------------------------------------------------

REM Build with cgsme_DEBUG and include the debug source so instrumentation is available
gcc -std=c11 -O3 -g -Dcgsme_DEBUG -D_GNU_SOURCE main.c generator.c cgsme_debug.c cgsme_utils.c cgsme_noise.c cgsme_topology.c cgsme_solver.c tinycthread/tinycthread.c -o debug_gen.exe -I. -pthread -lm
if %ERRORLEVEL% NEQ 0 (
    echo Compilation Failed!
    pause
    exit /b
)

echo Compilation Successful.

echo.
echo ---------------------------------------------------
echo 2. Running Generator...
echo ---------------------------------------------------

debug_gen.exe --cgsme-debug

echo.
echo ---------------------------------------------------
echo 3. Launching Visualizer...
echo ---------------------------------------------------

love .

echo Done.

popd