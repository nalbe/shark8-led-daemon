@echo off
rem ============================================================
rem  install_gui.cmd - build the LED GUI APK (release, R8
rem  minified, signed with the debug keystore), stage it into
rem  module\led_gui-release.apk (what customize.sh installs) and
rem  install it on a connected device. Builds the APK first if it
rem  is missing.
rem ============================================================
setlocal enabledelayedexpansion
set "GUI=%~dp0led_gui"
set "APK=%GUI%\app\build\outputs\apk\release\led_gui-release.apk"
set "DST=%~dp0module\led_gui-release.apk"

if not exist "%APK%" (
    echo APK not built - building with gradle...
    pushd "%GUI%"
    rem gradlew ships with the project, so it needs nothing from PATH.
    call gradlew.bat assembleRelease
    set "RC=!ERRORLEVEL!"
    popd
    if not "!RC!"=="0" (
        echo BUILD FAILED
        exit /b 1
    )
)

echo Waiting for device...
where adb >nul 2>nul
if errorlevel 1 goto :noadb
adb wait-for-device
if errorlevel 1 goto :noadb

copy /y "%APK%" "%DST%" >nul
if errorlevel 1 goto :fail
echo STAGED: %DST%

echo Installing %APK%
adb install -r "%APK%"
if errorlevel 1 goto :fail

echo Launching LED GUI...
adb shell am start -n com.bastet.ledgui/.MainActivity
echo INSTALLED AND LAUNCHED
exit /b 0

:noadb
echo adb not found in PATH - add platform-tools and reopen the terminal
exit /b 1

:fail
echo INSTALL FAILED - check USB debugging and unlock prompt on the device
exit /b 1