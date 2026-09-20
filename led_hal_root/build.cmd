@echo off
rem ============================================================
rem  build.cmd - compile the modular chgd daemon.
rem
rem  Compiles every .c in this directory (core) plus every .c in
rem  mods\ (extensions) and links the AW2033 chip controller as a static
rem  lib (lib\aw2033.h + lib\libaw2033.a - the two files the standalone
rem  aw2033-driver repo publishes, vendored so the daemon rebuilds
rem  without dragging the driver sources in here).
rem  Adding a feature = dropping a file into mods\ and running this.
rem
rem  Override the compiler with:   set NDK_CC=path\to\clang.cmd
rem ============================================================
setlocal enabledelayedexpansion
if not defined NDK_CC set "NDK_CC=D:\System\Apps\Android NDK\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android29-clang.cmd"

set SRC=
for %%f in ("%~dp0*.c") do set "SRC=!SRC! "%%f""
for %%f in ("%~dp0mods\*.c") do set "SRC=!SRC! "%%f""
if not defined SRC (
    echo ERROR: no C sources found in %~dp0 or %~dp0mods
    exit /b 1
)

set "AW=%~dp0..\lib"
if not exist "%AW%\libaw2033.a" (
    echo ERROR: %AW%\libaw2033.a not found - copy the prebuilt
    echo        binary + header from the standalone aw2033-driver
    echo        repo's release and drop them into lib\.
    exit /b 1
)

echo Compiling: %SRC%
call "%NDK_CC%" -O2 -s -Wall -Wno-comment -I"%AW%" -o "%~dp0chgd" %SRC% "%AW%\libaw2033.a"
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo BUILT: %~dp0chgd