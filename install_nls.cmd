@echo off
rem ============================================================
rem  install_nls.cmd - build the standalone NLS bridge APK and
rem  drop it into the module packaging dir (module\nls.apk).
rem  customize.sh installs nls.apk alongside led_gui.apk.
rem
rem  The bridge is a STANDALONE project now (projects\noty-bridge):
rem  this script only consumes its artifact. Fix NLS below if
rem  you keep the bridge repo elsewhere.
rem ============================================================
setlocal enabledelayedexpansion
set "NLS=%~dp0..\noty-bridge"
set "APK=%NLS%\app\build\outputs\apk\debug\app-debug.apk"
set "DST=%~dp0module\nls.apk"

if not exist "%APK%" (
    echo APK not built - building with gradle...
    pushd "%NLS%"
    where gradle >nul 2>nul
    if errorlevel 1 (
        call "D:\System\Apps\gradle-8.12\bin\gradle.bat" assembleDebug
    ) else (
        call gradle assembleDebug
    )
    set "RC=!ERRORLEVEL!"
    popd
    if not "!RC!"=="0" (
        echo BUILD FAILED
        exit /b 1
    )
)

copy /y "%APK%" "%DST%" >nul
if errorlevel 1 goto :fail

echo BUILT AND STAGED: %DST%
exit /b 0

:fail
echo BUILD FAILED - check Java/Gradle
exit /b 1