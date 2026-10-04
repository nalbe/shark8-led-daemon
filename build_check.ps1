<#
  build_check.ps1 - run a build that can never wedge the caller.

  Gradle's native stderr piped into a live PowerShell pipeline can block
  forever, and R8's dex output collides with a stale Gradle daemon lock.
  Both are handled here once, in one place: output goes to a log file (never
  a pipe), the process is killed at a hard timeout, and the caller gets a
  short verdict instead of a hang.

  Usage:
    .\build_check.ps1                       # assembleRelease in led_gui
    .\build_check.ps1 -Tasks clean,assembleRelease
    .\build_check.ps1 -TimeoutSec 1200
  Exit code: 0 = build ok, 1 = build failed, 2 = timeout, 3 = setup problem.
#>
param(
    [string[]]$Tasks = @("assembleRelease"),
    [int]$TimeoutSec = 900,
    [string]$WorkDir = "$PSScriptRoot\led_gui"
)

$ErrorActionPreference = "Stop"

$gradlew = Join-Path $WorkDir "gradlew.bat"
if (-not (Test-Path -LiteralPath $gradlew)) {
    Write-Output "SETUP ERROR: no gradlew.bat in $WorkDir"
    exit 3
}

$logDir = Join-Path $env:TEMP "opencode"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$out = Join-Path $logDir "build-$stamp.log"
$err = Join-Path $logDir "build-$stamp.err"

function Invoke-Build {
    $p = Start-Process -FilePath $gradlew `
        -ArgumentList ($Tasks + @("--console=plain")) `
        -WorkingDirectory $WorkDir -NoNewWindow -PassThru `
        -RedirectStandardOutput $out -RedirectStandardError $err
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill() } catch { }
        return 2
    }
    # ExitCode can read back empty on a redirected no-window process, so the
    # log's own verdict line is what decides pass or fail.
    if (Select-String -LiteralPath $out -Pattern "BUILD SUCCESSFUL" -Quiet) { return 0 }
    return 1
}

function Show-Verdict {
    $lines = @()
    if (Test-Path -LiteralPath $out) { $lines = Get-Content -LiteralPath $out }
    $bad = $lines | Select-String -Pattern "^e: |error:|What went wrong|Execution failed|BUILD FAILED" |
        Select-Object -First 8 | ForEach-Object { "  " + $_.Line.Trim() }
    $tail = $lines | Select-Object -Last 3 | ForEach-Object { "  " + $_ }
    if ($bad) { $bad } else { "  no error lines in the log" }
    "  --- tail ---"
    $tail
    "  log: $out"
}

$r = Invoke-Build

# R8 rewrites app\build\intermediates\dex in place; a daemon still holding the
# previous classes.dex makes that write fail. One visible retry, not a silent
# one: daemons go down and the dex intermediates are wiped.
if ($r -eq 1 -and (Test-Path -LiteralPath $out) -and
    (Select-String -LiteralPath $out -Pattern "being used by another process" -Quiet)) {
    Write-Output "R8 dex lock: stopping daemons, wiping app\build\intermediates\dex, retrying once"
    Start-Process -FilePath $gradlew -ArgumentList @("--stop") -WorkingDirectory $WorkDir `
        -NoNewWindow -Wait -RedirectStandardOutput $out -RedirectStandardError $err
    Start-Sleep -Seconds 2
    Remove-Item -Recurse -Force (Join-Path $WorkDir "app\build\intermediates\dex") -ErrorAction SilentlyContinue
    $out = Join-Path $logDir "build-$stamp-retry.log"
    $err = Join-Path $logDir "build-$stamp-retry.err"
    $r = Invoke-Build
}

switch ($r) {
    0 { Write-Output "BUILD OK"; exit 0 }
    2 { Write-Output "TIMEOUT after $TimeoutSec s - gradle killed"; Show-Verdict; exit 2 }
    default { Write-Output "BUILD FAILED"; Show-Verdict; exit 1 }
}