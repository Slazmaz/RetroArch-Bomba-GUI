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

# This script sits in the install's app\ folder, next to the retroarch.exe
# and retroarch.cfg it always runs; the install's own folders (settings,
# saves...) are one up, where bomba.exe starts this. Steam's launch option
# passes its own RetroArch first (%command%); that is replaced, keeping its
# arguments.
$app  = $PSScriptRoot
$root = Split-Path -Parent $app
$steamExe = $null
$argsToPass = @()

if ($Command -and $Command.Count -gt 0) {
    if ((Split-Path -Leaf $Command[0]) -ieq "retroarch.exe") {
        $steamExe = $Command[0]
        if ($Command.Count -gt 1) {
            $argsToPass = $Command[1..($Command.Count - 1)]
        }
    }
    else {
        $argsToPass = $Command
    }
}

# Steam starts the game with "+connect_lobby <id>" to join a friend's
# multiplayer session (Join Game, an accepted invite). RetroArch would take
# it for content; the fork reads the lobby from BOMBA_CONNECT_LOBBY.
$kept = @()
for ($i = 0; $i -lt $argsToPass.Count; $i++) {
    if ($argsToPass[$i] -eq "+connect_lobby" -and $i + 1 -lt $argsToPass.Count) {
        $env:BOMBA_CONNECT_LOBBY = $argsToPass[$i + 1]
        $i++
    }
    else {
        $kept += $argsToPass[$i]
    }
}
$argsToPass = $kept

$exe = Join-Path $app "retroarch.exe"

# Saves, save states and core settings live in Steam's RetroArch folder,
# where Steam Cloud backs them up: here they are links to it. Started by
# Steam, its RetroArch (%command%) says where that folder is. Started by
# hand, the links from the last Steam start stay; never started by Steam,
# they are ordinary folders here, whose files join Steam's on the first
# Steam start: where Steam's folder has the same file, Steam's stays and
# this one goes to <folder>_old (saves_old...).
if ($steamExe) {
    $steamRa = Split-Path -Parent $steamExe
    foreach ($dir in "saves", "states", "config") {
        $link   = Join-Path $root $dir
        $target = Join-Path $steamRa $dir
        $old    = Join-Path $root "${dir}_old"
        New-Item -ItemType Directory -Force $target | Out-Null
        $item = Get-Item -LiteralPath $link -Force -ErrorAction SilentlyContinue
        if ($item -and -not $item.LinkType) {
            $ok = $true
            foreach ($f in Get-ChildItem -LiteralPath $link -Recurse -File -Force) {
                $rel  = $f.FullName.Substring($link.Length + 1)
                $dest = Join-Path $target $rel
                if (Test-Path -LiteralPath $dest) { $dest = Join-Path $old $rel }
                New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
                try { Move-Item -LiteralPath $f.FullName -Destination $dest -ErrorAction Stop }
                catch { $ok = $false }
            }
            # Anything left (a file in use): keep the folder as it is.
            if (-not $ok) { continue }
            Remove-Item -LiteralPath $link -Recurse -Force
        }
        elseif ($item) {
            if (($item.Target -join "") -eq $target) { continue }
            cmd /c rmdir "$link" | Out-Null # removes only the link
        }
        cmd /c mklink /J "$link" "$target" | Out-Null
    }
}

# Tell the user (via the fork's on-screen messages) when Steam has shipped
# a newer RetroArch than this build is based on.
if ($steamExe) {
    $steamBuild = Join-Path (Split-Path -Parent $steamExe) "build.txt"
    $steamRev = Select-String -Path $steamBuild -Pattern '^Rev:\s*(\S+)' -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Matches[0].Groups[1].Value } |
        Select-Object -First 1
    $buildRevFile = Join-Path $app "build-rev.txt"
    $buildRev = if (Test-Path $buildRevFile) { (Get-Content $buildRevFile -TotalCount 1).Trim() }

    if ($steamRev -and $buildRev -and $steamRev -ne $buildRev) {
        $env:BOMBA_MSG = "RetroArch update available"
    }
}

if (-not ($argsToPass -contains "--config" -or $argsToPass -contains "-c")) {
    $argsToPass = @("--config", (Join-Path $app "retroarch.cfg")) + $argsToPass
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
