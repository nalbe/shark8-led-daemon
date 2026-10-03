@echo off
rem ============================================================
rem  build_module.cmd - unified release build.
rem
rem  Compiles chgd (led_hal_root\build.cmd), then assembles the
rem  flashable KernelSU module zip into release\led_hal_root-v<ver>.zip
rem  from exactly two sources:
rem    led_hal_root\   daemon core only   (C sources, mods\, build.cmd)
rem    module\         module packaging   (customize.sh, service.sh,
rem                      module.prop, led.conf, META-INF,
rem                      notifybridge-release.apk,
rem                      led_gui-release.apk,
rem                      awctl prebuilt)
rem  The zip is the union of those two folders - nothing else.
rem  awctl is NOT rebuilt here: the binary in module\ comes prebuilt
rem  from the standalone aw2033-driver repo (sources + build there;
rem  this project ships it as-is).
rem
rem  Compiler: set NDK_CC to your NDK clang wrapper before running
rem  (build.cmd errors out on an unset NDK_CC too). Nothing is
rem  hardcoded here - a machine-local NDK path is not portable.
rem ============================================================
setlocal

set "ROOT=%~dp0"
set "CORE=%ROOT%led_hal_root"
set "PKG=%ROOT%module"
set "STAGE=%TEMP%\led_hal_root_build"

for /f "tokens=2 delims==" %%v in ('findstr /b "version=" "%PKG%\module.prop"') do set "VER=%%v"
if not defined VER (
    echo ERROR: cannot read version from module\module.prop
    exit /b 1
)

if not exist "%PKG%\awctl" (
    echo ERROR: %PKG%\awctl missing - copy the prebuilt binary from
    echo        the standalone aw2033-driver repo first.
    exit /b 1
)

echo [1/3] Building chgd...
call "%CORE%\build.cmd"
if errorlevel 1 exit /b 1

echo [2/3] Staging module tree...
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
xcopy "%CORE%\*" "%STAGE%\" /e /y /q >nul
if errorlevel 1 exit /b 1
xcopy "%PKG%\*" "%STAGE%\" /e /y /q >nul
if errorlevel 1 exit /b 1

echo [3/3] Packaging release\led_hal_root-v%VER%.zip...
if not exist "%ROOT%release" mkdir "%ROOT%release"
if exist "%ROOT%release\led_hal_root-v%VER%.zip" del /q "%ROOT%release\led_hal_root-v%VER%.zip"
rem Zip entry names must use '/', so the archive is built entry by entry.
powershell -NoProfile -Command ^
  "Add-Type -AssemblyName System.IO.Compression;" ^
  "Add-Type -AssemblyName System.IO.Compression.FileSystem;" ^
  "$stage='%STAGE%'; $zip='%ROOT%release\led_hal_root-v%VER%.zip';" ^
  "$fs=[IO.File]::Open($zip,'Create');" ^
  "$ar=New-Object IO.Compression.ZipArchive($fs,[IO.Compression.ZipArchiveMode]::Create);" ^
  "Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {" ^
  "  $rel=$_.FullName.Substring($stage.Length).TrimStart('\','/').Replace('\','/');" ^
  "  [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($ar,$_.FullName,$rel,[IO.Compression.CompressionLevel]::Optimal) | Out-Null }; " ^
  "$ar.Dispose(); $fs.Dispose()"
if errorlevel 1 (
    echo ZIP FAILED
    rmdir /s /q "%STAGE%" 2>nul
    exit /b 1
)

rmdir /s /q "%STAGE%"
echo DONE: %ROOT%release\led_hal_root-v%VER%.zip
exit /b 0