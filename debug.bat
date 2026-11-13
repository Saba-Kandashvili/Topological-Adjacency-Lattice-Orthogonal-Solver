@echo off
REM Ensure script runs from its own directory so relative paths resolve
pushd %~dp0

set "TILESET_FILE=%CGSME_TILESET%"
set "GEN_ARGS=%*"
if not "%~1"=="" (
    if /I "%~x1"==".json" (
        set "TILESET_FILE=%~1"
        for /f "tokens=1*" %%A in ("%*") do set "GEN_ARGS=%%B"
    )
)
if "%TILESET_FILE%"=="" set "TILESET_FILE=tileset.json"

echo ---------------------------------------------------
echo 0. Baking %TILESET_FILE% -^> cgsme_tileset_baked.h...
echo ---------------------------------------------------
gcc -std=c11 -O2 cgsme_codegen.c cgsme_ruleset.c -o cgsme_codegen.exe
if %ERRORLEVEL% NEQ 0 (
    echo Tileset codegen compile failed!
    pause
    exit /b
)
cgsme_codegen.exe "%TILESET_FILE%" cgsme_tileset_baked.h
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

debug_gen.exe --cgsme-debug %GEN_ARGS%

echo.
echo ---------------------------------------------------
echo 3. Launching Visualizer...
echo ---------------------------------------------------

set "CGSME_TILESET=%TILESET_FILE%"
love .

echo Done.

popd
