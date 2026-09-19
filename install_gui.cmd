@echo off
rem ============================================================
rem  install_gui.cmd - install the LED GUI app to a connected
rem  device. Builds the APK first if it is missing.
rem ============================================================
setlocal enabledelayedexpansion
set "GUI=%~dp0led_gui"
set "APK=%GUI%\app\build\outputs\apk\debug\app-debug.apk"

if not exist "%APK%" (
    echo APK not built - building with gradle...
    pushd "%GUI%"
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

echo Waiting for device...
adb wait-for-device
if errorlevel 1 goto :noadb

echo Installing %APK%
adb install -r "%APK%"
if errorlevel 1 goto :fail

echo Launching LED GUI...
adb shell am start -n com.bastet.ledgui/.MainActivity
echo INSTALLED AND LAUNCHED
exit /b 0

:noadb
echo adb not found in PATH (added: D:\System\Apps\adb - reopen the terminal)
exit /b 1

:fail
echo INSTALL FAILED - check USB debugging and unlock prompt on the device
exit /b 1