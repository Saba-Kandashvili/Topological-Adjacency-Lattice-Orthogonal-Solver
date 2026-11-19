@echo off
setlocal EnableDelayedExpansion
set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%.."
set "ROOT_DIR=%CD%"
popd
set "CALLER_DIR=%CD%"

set "TILESET_FILE=%CGSME_TILESET%"
if "%TILESET_FILE%"=="" set "TILESET_FILE=%ROOT_DIR%\tileset.json"
set "SOFT_MODE=0"
set "FORWARD_ARGS="

:parse_args
if "%~1"=="" goto done_args
if /I "%~1"=="--soft" (
    set "SOFT_MODE=1"
    if not "%~2"=="" (
        if /I "%~x2"==".json" (
            if exist "%~2" (
                set "TILESET_FILE=%~f2"
            ) else if exist "%ROOT_DIR%\%~2" (
                set "TILESET_FILE=%ROOT_DIR%\%~2"
            ) else (
                set "TILESET_FILE=%~2"
            )
            shift
        )
    )
    shift
    goto parse_args
)
if /I "%~1"=="-s" (
    set "SOFT_MODE=1"
    if not "%~2"=="" (
        if /I "%~x2"==".json" (
            if exist "%~2" (
                set "TILESET_FILE=%~f2"
            ) else if exist "%ROOT_DIR%\%~2" (
                set "TILESET_FILE=%ROOT_DIR%\%~2"
            ) else (
                set "TILESET_FILE=%~2"
            )
            shift
        )
    )
    shift
    goto parse_args
)
if /I "%~x1"==".json" (
    if exist "%~1" (
        set "TILESET_FILE=%~f1"
        shift
        goto parse_args
    ) else if exist "%ROOT_DIR%\%~1" (
        set "TILESET_FILE=%ROOT_DIR%\%~1"
        shift
        goto parse_args
    )
)
set "FORWARD_ARGS=!FORWARD_ARGS! %1"
shift
goto parse_args
:done_args

pushd "%SCRIPT_DIR%"

if "%SOFT_MODE%"=="1" (
    echo ---------------------------------------------------
    echo 0. Soft Mode Enabled ^(%TILESET_FILE%^) - Skipping Codegen
    echo ---------------------------------------------------
    if not exist "%ROOT_DIR%\cgsme_tileset_baked.h" (
        gcc -std=c11 -O2 "%ROOT_DIR%\cgsme_codegen.c" "%ROOT_DIR%\cgsme_ruleset.c" -I"%ROOT_DIR%" -o cgsme_codegen.exe
        cgsme_codegen.exe "%ROOT_DIR%\tileset.json" "%ROOT_DIR%\cgsme_tileset_baked.h"
    )
    echo ---------------------------------------------------
    echo 1. Compiling C Code ^(Soft Mode - No Header Rebake^)...
    echo ---------------------------------------------------
    gcc -std=c11 -O3 -g -Dcgsme_DEBUG -D_GNU_SOURCE main.c "%ROOT_DIR%\cgsme_ruleset.c" "%ROOT_DIR%\cgsme_soft.c" "%ROOT_DIR%\generator.c" "%ROOT_DIR%\cgsme_debug.c" "%ROOT_DIR%\cgsme_utils.c" "%ROOT_DIR%\cgsme_noise.c" "%ROOT_DIR%\cgsme_topology.c" "%ROOT_DIR%\cgsme_solver.c" "%ROOT_DIR%\tinycthread\tinycthread.c" -o debug_gen.exe -I"%ROOT_DIR%" -pthread -lm
    if !ERRORLEVEL! NEQ 0 (
        echo Compilation Failed!
        pause
        popd
        exit /b 1
    )
    echo.
    echo ---------------------------------------------------
    echo 2. Running Generator ^(--soft %TILESET_FILE%^)...
    echo ---------------------------------------------------
    debug_gen.exe --cgsme-debug --soft "%TILESET_FILE%" !FORWARD_ARGS!
) else (
    echo ---------------------------------------------------
    echo 0. Baking %TILESET_FILE% -^> cgsme_tileset_baked.h...
    echo ---------------------------------------------------
    gcc -std=c11 -O2 "%ROOT_DIR%\cgsme_codegen.c" "%ROOT_DIR%\cgsme_ruleset.c" -I"%ROOT_DIR%" -o cgsme_codegen.exe
    if !ERRORLEVEL! NEQ 0 (
        echo Tileset codegen compile failed!
        pause
        popd
        exit /b 1
    )
    cgsme_codegen.exe "%TILESET_FILE%" "%ROOT_DIR%\cgsme_tileset_baked.h"
    if !ERRORLEVEL! NEQ 0 (
        echo Tileset baking failed!
        pause
        popd
        exit /b 1
    )
    echo.
    echo ---------------------------------------------------
    echo 1. Compiling C Code ^(Baked + Soft Mode^)...
    echo ---------------------------------------------------
    gcc -std=c11 -O3 -g -Dcgsme_DEBUG -D_GNU_SOURCE main.c "%ROOT_DIR%\cgsme_ruleset.c" "%ROOT_DIR%\cgsme_soft.c" "%ROOT_DIR%\generator.c" "%ROOT_DIR%\cgsme_debug.c" "%ROOT_DIR%\cgsme_utils.c" "%ROOT_DIR%\cgsme_noise.c" "%ROOT_DIR%\cgsme_topology.c" "%ROOT_DIR%\cgsme_solver.c" "%ROOT_DIR%\tinycthread\tinycthread.c" -o debug_gen.exe -I"%ROOT_DIR%" -pthread -lm
    if !ERRORLEVEL! NEQ 0 (
        echo Compilation Failed!
        pause
        popd
        exit /b 1
    )
    echo Compilation Successful.
    echo.
    echo ---------------------------------------------------
    echo 2. Running Generator ^(Baked^)...
    echo ---------------------------------------------------
    debug_gen.exe --cgsme-debug !FORWARD_ARGS!
)

echo.
echo ---------------------------------------------------
echo 3. Launching Visualizer...
echo ---------------------------------------------------

set "CGSME_TILESET=%TILESET_FILE%"
love .

echo Done.

popd
