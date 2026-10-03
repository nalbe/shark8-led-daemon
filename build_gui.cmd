@echo off
rem ============================================================
rem  build_gui.cmd - release build of the LED GUI APK.
rem
rem  Runs `assembleRelease` in led_gui\ (R8 minified, signed with
rem  the debug keystore - this is a side-loaded configurator, not a
rem  Play app), then stages the result into module\led_gui-release.apk.
rem  That staged file is what build_module.cmd packs into the module
rem  zip and what customize.sh installs, so this script is the GUI
rem  half of the release and build_module.cmd the packaging half.
rem
rem  Launcher, first hit wins (each choice is echoed, never silent):
rem    1. led_gui\gradlew.bat   - the committed wrapper, reproducible
rem    2. gradle on PATH       - whatever you have installed
rem    3. nothing              - hard error, no machine-local guess
rem
rem  The Android SDK comes from led_gui\local.properties (sdk.dir);
rem  set ANDROID_HOME as a fallback when that file is absent.
rem
rem  Usage:  build_gui.cmd            build + stage
rem          build_gui.cmd --clean    wipe app\build first (forces R8
rem                                      to re-read every source file)
rem ============================================================
rem enabledelayedexpansion: the launcher pick runs inside nested if/else
rem blocks, where a plain %VAR% would be expanded before the set.
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
set "GUI=%ROOT%led_gui"
set "APK=%GUI%\app\build\outputs\apk\release\led_gui-release.apk"
set "DST=%ROOT%module\led_gui-release.apk"

rem clean is a TASK, not a flag: `assembleRelease --clean` is a task-graph
rem error, `clean assembleRelease` is the real thing. The rmdir on top of it
rem removes what R8 wrote, so no stale merged resources survive the rebuild.
set "TASKS=assembleRelease"
if /i "%~1"=="--clean" set "TASKS=clean assembleRelease"

rem The version lives in exactly one place: build.gradle.kts. Read it for
rem the report line so the log always says what was actually built.
rem The keys are indented in build.gradle.kts, so match anywhere on the
rem line and split on '=' - findstr /b would miss them. The value comes
rem back padded and quoted (" 1.5"), hence the two substitutions.
set "VER="
set "CODE="
for /f "tokens=2 delims==" %%v in ('findstr /c:"versionName" "%GUI%\app\build.gradle.kts"') do set "VER=%%v"
for /f "tokens=2 delims==" %%v in ('findstr /c:"versionCode" "%GUI%\app\build.gradle.kts"') do set "CODE=%%v"
if not defined VER (
    echo ERROR: cannot read versionName from %GUI%\app\build.gradle.kts
    exit /b 1
)
set "VER=!VER: =!"
set "VER=!VER:"=!"
set "CODE=!CODE: =!"

if not exist "%GUI%\local.properties" if not defined ANDROID_HOME (
    echo ERROR: neither led_gui\local.properties nor ANDROID_HOME is set -
    echo        gradle cannot find the Android SDK.
    exit /b 1
)

rem ROOT ends in a backslash, so every GUI path needs its own separator.
if exist "%GUI%\gradlew.bat" (
    set "GRADLE=%GUI%\gradlew.bat"
    echo Launcher: gradlew.bat ^(committed wrapper^)
) else (
    where gradle >nul 2>nul
    if not errorlevel 1 (
        set "GRADLE=gradle"
        echo Launcher: gradle from PATH
    ) else (
        echo ERROR: no gradle launcher found - led_gui\gradlew.bat is
        echo        missing and gradle is not on PATH. Fix one of the two.
        exit /b 1
    )
)

rem No bare parentheses inside these echo lines: an unescaped ) closes
rem the block early and cmd then reports "unexpected at this time".
if /i "%~1"=="--clean" (
    echo [1/2] Building GUI %VER% code %CODE% from clean...
    if exist "%GUI%\app\build" rmdir /s /q "%GUI%\app\build"
) else (
    echo [1/2] Building GUI %VER% code %CODE%...
)

rem --offline is NOT the default: the dependency set changes whenever a
rem Gradle file is edited, and an uncached artifact then fails the release
rem over a lookup that a normal build would just make. Set GRADLE_OFFLINE=1
rem once everything is cached if you want a network-free build.
set "OFFLINE="
if /i "%GRADLE_OFFLINE%"=="1" set "OFFLINE=--offline"
pushd "%GUI%"
call "%GRADLE%" %TASKS% %OFFLINE%
set "RC=!ERRORLEVEL!"
popd
if not "!RC!"=="0" (
    echo BUILD FAILED, gradle exit !RC!
    exit /b 1
)

if not exist "%APK%" (
    echo ERROR: build reported success but %APK% is missing
    exit /b 1
)

rem Stage into module\ - the single copy customize.sh installs and
rem build_module.cmd packages. A stale one there would ship the previous
rem GUI under the new version, so this is not optional.
echo [2/2] Staging into module\led_gui-release.apk...
if not exist "%ROOT%module" (
    echo ERROR: %ROOT%module missing - this script expects the repo layout.
    exit /b 1
)
copy /y "%APK%" "%DST%" >nul
if errorlevel 1 (
    echo STAGE FAILED
    exit /b 1
)

for %%a in ("%DST%") do set "SIZE=%%~za"
echo DONE: GUI %VER% (code %CODE%), %SIZE% bytes
echo       %DST%
echo Next: build_module.cmd packages the module zip,
echo        install_gui.cmd installs the APK on a device
exit /b 0
