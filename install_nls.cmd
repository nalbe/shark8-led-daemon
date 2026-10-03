@echo off
rem ============================================================
rem  install_nls.cmd - build the standalone NLS bridge APK
rem  (release, R8 minified, signed with the debug keystore) and
rem  drop it into the module packaging dir
rem  (module\notifybridge-release.apk). customize.sh installs
rem  notifybridge-release.apk alongside led_gui-release.apk.
rem
rem  The bridge is a STANDALONE project (sibling dir
rem  ..\android-notify-bridge by default): this script only consumes
rem  its artifact. Set NLS to the bridge repo if you keep it elsewhere:
rem    set NLS=D:\somewhere\android-notify-bridge
rem ============================================================
setlocal enabledelayedexpansion
if "%NLS%"=="" set "NLS=%~dp0..\android-notify-bridge"
if not exist "%NLS%" (
    echo ERROR: bridge project not found: %NLS%
    echo   set NLS to the android-notify-bridge repo and rerun.
    exit /b 1
)
set "APK=%NLS%\app\build\outputs\apk\release\notifybridge-release.apk"
set "DST=%~dp0module\notifybridge-release.apk"

rem Always rebuild so staged code is never stale on a rerun.
echo Building notifybridge-release.apk...
pushd "%NLS%"
rem gradlew ships with the bridge project, so it needs nothing from PATH.
call gradlew.bat assembleRelease
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