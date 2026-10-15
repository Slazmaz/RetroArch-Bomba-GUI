param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Command
)

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class WindowHelper
{
    [DllImport("user32.dll")]
    public static extern bool ShowWindowAsync(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool BringWindowToTop(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(
        IntPtr hWnd,
        IntPtr lpdwProcessId
    );

    [DllImport("kernel32.dll")]
    public static extern uint GetCurrentThreadId();

    [DllImport("user32.dll")]
    public static extern bool AttachThreadInput(
        uint idAttach,
        uint idAttachTo,
        bool fAttach
    );

    public static void ForceForeground(IntPtr hWnd)
    {
        IntPtr foreground = GetForegroundWindow();

        uint currentThread = GetCurrentThreadId();
        uint foregroundThread =
            GetWindowThreadProcessId(foreground, IntPtr.Zero);
        uint targetThread =
            GetWindowThreadProcessId(hWnd, IntPtr.Zero);

        bool attachedForeground = false;
        bool attachedTarget = false;

        try
        {
            if (foregroundThread != 0 &&
                foregroundThread != currentThread)
            {
                attachedForeground = AttachThreadInput(
                    currentThread,
                    foregroundThread,
                    true
                );
            }

            if (targetThread != 0 &&
                targetThread != currentThread)
            {
                attachedTarget = AttachThreadInput(
                    currentThread,
                    targetThread,
                    true
                );
            }

            ShowWindowAsync(hWnd, 3);
            BringWindowToTop(hWnd);
            SetForegroundWindow(hWnd);
        }
        finally
        {
            if (attachedTarget)
            {
                AttachThreadInput(
                    currentThread,
                    targetThread,
                    false
                );
            }

            if (attachedForeground)
            {
                AttachThreadInput(
                    currentThread,
                    foregroundThread,
                    false
                );
            }
        }
    }
}
"@

if (-not $Command -or $Command.Count -eq 0) {
    exit 1
}

$exe = $Command[0]
$argsToPass = @()

if ($Command.Count -gt 1) {
    $argsToPass = $Command[1..($Command.Count - 1)]
}

# Swap Steam's RetroArch for the custom build (see build.sh) when present.
# Data folders in build\ are junctions into Steam's install, and the config
# is Steam's own, so saves, cores and settings stay shared.
# This script lives in wrapper\scripts; $root is the repo root.
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$customExe = Join-Path $root "build\retroarch.exe"

if (
    (Split-Path -Leaf $exe) -ieq "retroarch.exe" -and
    (Test-Path $customExe)
) {
    $steamDir = Split-Path -Parent $exe
    $steamConfig = Join-Path $steamDir "retroarch.cfg"

    # Tell the user (via the fork's on-screen messages) when Steam has
    # shipped a newer RetroArch than the custom build is based on.
    $steamRev = Select-String -Path (Join-Path $steamDir "build.txt") -Pattern '^Rev:\s*(\S+)' |
        ForEach-Object { $_.Matches[0].Groups[1].Value } |
        Select-Object -First 1
    $buildRevFile = Join-Path $root "build\build-rev.txt"
    $buildRev = if (Test-Path $buildRevFile) { (Get-Content $buildRevFile -TotalCount 1).Trim() }

    if ($steamRev -and $steamRev -ne $buildRev) {
        $env:RA_WRAPPER_MSG = "RetroArch update available - run wrapper\scripts\update.cmd"
    }

    if (-not ($argsToPass -contains "--config" -or $argsToPass -contains "-c")) {
        $argsToPass = @("--config", $steamConfig) + $argsToPass
    }

    $exe = $customExe
}

$workingDir = Split-Path -Parent $exe

if ($argsToPass.Count -gt 0) {
    # Start-Process doesn't quote array args, so paths with spaces would split.
    $argString = ($argsToPass | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }) -join " "

    $process = Start-Process `
        -FilePath $exe `
        -ArgumentList $argString `
        -WorkingDirectory $workingDir `
        -PassThru
}
else {
    $process = Start-Process `
        -FilePath $exe `
        -WorkingDirectory $workingDir `
        -PassThru
}

if (-not $process) {
    exit 1
}

$lastWindow = [IntPtr]::Zero

while (-not $process.HasExited) {

    $process.Refresh()
    $currentWindow = $process.MainWindowHandle

    if (
        $currentWindow -ne [IntPtr]::Zero -and
        $currentWindow -ne $lastWindow
    ) {
        # Give RetroArch enough time to finish creating
        # and activating the newly created window.
        Start-Sleep -Milliseconds 500

        $process.Refresh()
        $currentWindow = $process.MainWindowHandle

        if ($currentWindow -ne [IntPtr]::Zero) {

            [WindowHelper]::ForceForeground(
                $currentWindow
            )

            # Hit it a second time shortly afterward.
            # Steam sometimes retakes focus immediately after launch.
            Start-Sleep -Milliseconds 300

            $process.Refresh()

            if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
                [WindowHelper]::ForceForeground(
                    $process.MainWindowHandle
                )
            }

            $lastWindow = $currentWindow
        }
    }

    Start-Sleep -Milliseconds 100
}

exit $process.ExitCode
