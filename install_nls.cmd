@echo off
rem ============================================================
rem  install_nls.cmd - build the standalone NLS bridge APK
rem  (release, R8 minified, signed with the debug keystore) and
rem  drop it into the module packaging dir
rem  (module\notifybridge-release.apk). customize.sh installs
rem  notifybridge-release.apk alongside led_gui-release.apk.
rem
rem  The bridge is a STANDALONE project now
rem  (projects\android-notify-bridge): this script only consumes its
rem  artifact. Fix NLS below if you keep the bridge repo elsewhere.
rem ============================================================
setlocal enabledelayedexpansion
set "NLS=%~dp0..\android-notify-bridge"
set "APK=%NLS%\app\build\outputs\apk\release\notifybridge-release.apk"
set "DST=%~dp0module\notifybridge-release.apk"

rem Always rebuild so staged code is never stale on a rerun.
echo Building notifybridge-release.apk...
pushd "%NLS%"
where gradle >nul 2>nul
if errorlevel 1 (
    call "D:\System\Apps\gradle-8.12\bin\gradle.bat" assembleRelease
) else (
    call gradle assembleRelease
)
set "RC=!ERRORLEVEL!"
popd
if not "!RC!"=="0" (
    echo BUILD FAILED
    exit /b 1
)

copy /y "%APK%" "%DST%" >nul
if errorlevel 1 goto :fail

echo BUILT AND STAGED: %DST%
exit /b 0

:fail
echo BUILD FAILED - check Java/Gradle
exit /b 1