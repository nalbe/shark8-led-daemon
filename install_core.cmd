@echo off
rem led_hal_root v4.1 apply script: build, push module files, restart the stack.
rem Requires: adb on PATH (or set ADB to its full path), adbd root, KernelSU.
setlocal

if "%ADB%"=="" set ADB=adb
where "%ADB%" >nul 2>&1
if errorlevel 1 (
  echo adb not found. Put platform-tools on PATH or set ADB to its full path.
  exit /b 1
)
set MOD=/data/adb/modules/led_hal_root
set SRC=%~dp0led_hal_root
set PKG=%~dp0module

echo Rebuilding chgd from sources...
call "%SRC%\build.cmd"
if errorlevel 1 goto fail

echo Pushing module files...
%ADB% push "%SRC%\chgd" %MOD%/chgd
if errorlevel 1 goto fail
%ADB% push "%SRC%\chgd.h" %MOD%/chgd.h
%ADB% push "%SRC%\core.c" %MOD%/core.c
%ADB% push "%SRC%\led.c" %MOD%/led.c
%ADB% push "%SRC%\config.c" %MOD%/config.c
%ADB% push "%SRC%\channel.c" %MOD%/channel.c
%ADB% push "%SRC%\util.c" %MOD%/util.c
rem Wipe the target first so `push mods` cannot layer stale files, then push.
%ADB% shell "rm -rf %MOD%/mods"
%ADB% push "%SRC%\mods" %MOD%/mods
rem Runtime config is USER state - never overwrite it. The repo copy is
rem the shipped template. install_core.cmd backs up the old file first,
rem then pushes the fresh template over it (a dev-apply may be the first
rem deploy, so no "adopt only when missing" here - the .bak is the safety).
%ADB% shell "test -f %MOD%/led.conf && cp -f %MOD%/led.conf %MOD%/led.conf.bak || true"
%ADB% push "%PKG%\led.conf" %MOD%/led.conf
rem The bridge config ships with the module too (repo copy = canonical,
rem always overwritten, same policy as led.conf). The bridge re-reads it
rem on (re)bind - RELOAD_CONFIG is unreliable on a chilled process, so
rem the restart below force-stops the app to force the rebind.
%ADB% push "%PKG%\notifybridge.json" /data/local/tmp/notifybridge.json
%ADB% push "%PKG%\service.sh" %MOD%/service.sh
%ADB% push "%PKG%\module.prop" %MOD%/module.prop

%ADB% shell "rm -f %MOD%/tele.c %MOD%/notify.c %MOD%/charge.c %MOD%/ring.c %MOD%/dialer.c"
%ADB% shell "rm -f %MOD%/keepalive.sh"

echo Restarting stack...
%ADB% shell "am force-stop com.bastet.notifybridge; kill -9 $(pidof chgd) 2>/dev/null; sleep 1; rm -f /data/local/tmp/ledd.log; chmod 755 %MOD%/chgd %MOD%/service.sh; setsid sh %MOD%/service.sh; sleep 1; pidof chgd"
echo Done. Check: %ADB% shell tail -n 5 /data/local/tmp/ledd.log
exit /b 0

:fail
echo BUILD OR PUSH FAILED - is the device connected?
exit /b 1