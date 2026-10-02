<#
  UI test helpers for attome-editor on Windows (PowerShell 5.1).

  Starts a separate editor instance with its own daemon and its own project, so a test never touches the user's open
  editor or projects, then drives it with the real mouse and keyboard and takes full-size window captures.

    Import-Module .\tools\uitest\uitest.psm1
    $ed = Start-Editor -Project C:\temp\T.attome -SelectFirstClip
    Show-Window $ed                      # bring it to the front (topmost) for input and capture
    Move-Drag $ed 853 320 703 425        # window-relative coordinates, as in Save-Shot output
    Save-Shot $ed C:\temp\after.png      # full-size capture of the editor window
    Invoke-Attome $ed get C:\temp\T.attome clp_...   # the CLI, pointed at this editor's daemon
    Stop-Editor $ed

  Rules: input goes only to points inside the editor window; the instance is closed at the end; the user's own
  editor (the default daemon endpoint) is never used. Coordinates are in pixels of the captured image.
#>

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class UiT {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int cx, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  public struct R { public int L, T, Rr, B; }
}
"@
[UiT]::SetProcessDPIAware() | Out-Null

$script:Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Get-BinDir { Join-Path $script:Root 'build\win-msvc-release\bin' }

# The CLI against this editor's own daemon.
function Invoke-Attome {
  param($Editor, [Parameter(ValueFromRemainingArguments = $true)] $Args)
  $old = $env:ATTOME_ENDPOINT; $env:ATTOME_ENDPOINT = $Editor.Endpoint
  try { & (Join-Path (Get-BinDir) 'attome.exe') @Args } finally { $env:ATTOME_ENDPOINT = $old }
}

function Start-Editor {
  param([Parameter(Mandatory)][string]$Project, [switch]$SelectFirstClip, [int]$WaitSeconds = 5, [string[]]$Import = @())
  $endpoint = "\\.\pipe\attome-uitest-$PID-$([guid]::NewGuid().ToString('N').Substring(0, 6))"
  $oldE = $env:ATTOME_ENDPOINT; $oldS = $env:ATTOME_EDITOR_SELECT
  $env:ATTOME_ENDPOINT = $endpoint
  if ($SelectFirstClip) { $env:ATTOME_EDITOR_SELECT = '1' } else { Remove-Item Env:\ATTOME_EDITOR_SELECT -ErrorAction SilentlyContinue }
  $editorArgs = (@($Project) + $Import | ForEach-Object { "`"$_`"" }) -join ' ' # media files are imported on start
  try { $p = Start-Process (Join-Path (Get-BinDir) 'attome-editor.exe') -ArgumentList $editorArgs -PassThru }
  finally { $env:ATTOME_ENDPOINT = $oldE; if ($oldS) { $env:ATTOME_EDITOR_SELECT = $oldS } else { Remove-Item Env:\ATTOME_EDITOR_SELECT -ErrorAction SilentlyContinue } }
  Start-Sleep -Seconds $WaitSeconds
  $p.Refresh()
  [pscustomobject]@{ Process = $p; Endpoint = $endpoint; Project = $Project }
}

function Get-Rect($Editor) {
  $Editor.Process.Refresh()
  $r = New-Object UiT+R
  [UiT]::GetWindowRect($Editor.Process.MainWindowHandle, [ref]$r) | Out-Null
  $r
}

function Show-Window($Editor) {
  $h = $Editor.Process.MainWindowHandle
  [UiT]::ShowWindow($h, 9) | Out-Null
  [UiT]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 3) | Out-Null   # topmost, so nothing covers it
  [UiT]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 800
  # The editor opens maximized; the scripts' coordinates are for the restored 1600 x 960 window, so wait for it.
  for ($i = 0; $i -lt 25; $i++) {
    $r = Get-Rect $Editor
    if (($r.Rr - $r.L) -ge 1500 -and ($r.Rr - $r.L) -le 1700) { break }
    [UiT]::ShowWindow($Editor.Process.MainWindowHandle, 9) | Out-Null
    Start-Sleep -Milliseconds 200
  }
  Start-Sleep -Milliseconds 300
}

function Save-Shot($Editor, [string]$Path) {
  $r = Get-Rect $Editor
  $w = $r.Rr - $r.L; $h = $r.B - $r.T
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
  $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  "$w x $h"
}

function Assert-Inside($Editor, $x, $y) {
  $r = Get-Rect $Editor
  if ($x -lt 0 -or $y -lt 0 -or $x -ge ($r.Rr - $r.L) -or $y -ge ($r.B - $r.T)) {
    throw "Refusing to send input outside the editor window: ($x, $y)"
  }
}

function Move-Mouse($Editor, $x, $y) {
  Assert-Inside $Editor $x $y
  $r = Get-Rect $Editor
  [UiT]::SetCursorPos($r.L + $x, $r.T + $y) | Out-Null
}

function Click-At($Editor, $x, $y) {
  Move-Mouse $Editor $x $y; Start-Sleep -Milliseconds 300
  [UiT]::mouse_event(2, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 100
  [UiT]::mouse_event(4, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 500
}

# Press, move in small steps (the UI samples the mouse once per frame), release.
function Move-Drag($Editor, $x0, $y0, $x1, $y1, [int]$Steps = 15, [switch]$HoldAtEnd) {
  Move-Mouse $Editor $x0 $y0; Start-Sleep -Milliseconds 400
  [UiT]::mouse_event(2, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 200
  for ($i = 1; $i -le $Steps; $i++) {
    Move-Mouse $Editor ([int]($x0 + ($x1 - $x0) * $i / $Steps)) ([int]($y0 + ($y1 - $y0) * $i / $Steps))
    Start-Sleep -Milliseconds 40
  }
  Start-Sleep -Milliseconds 300
  if (-not $HoldAtEnd) { [UiT]::mouse_event(4, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 800 }
}
function Release-Mouse { [UiT]::mouse_event(4, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 800 }

# Mouse wheel over a point; positive notches scroll down.
function Scroll-At($Editor, $x, $y, [int]$Notches) {
  Move-Mouse $Editor $x $y; Start-Sleep -Milliseconds 300
  [UiT]::mouse_event(0x0800, 0, 0, [BitConverter]::ToUInt32([BitConverter]::GetBytes([int32]($Notches * -120)), 0), [IntPtr]::Zero)
  Start-Sleep -Milliseconds 600
}

# Virtual-key codes, e.g. 0x20 space, 0x2E delete, 0x53 S. Add 'ctrl' for Ctrl+key.
function Send-Key($Editor, [byte]$Vk, [switch]$Ctrl) {
  if ($Ctrl) { [UiT]::keybd_event(0x11, 0, 0, [IntPtr]::Zero) }
  [UiT]::keybd_event($Vk, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 60
  [UiT]::keybd_event($Vk, 0, 2, [IntPtr]::Zero)
  if ($Ctrl) { [UiT]::keybd_event(0x11, 0, 2, [IntPtr]::Zero) }
  Start-Sleep -Milliseconds 500
}

function Stop-Editor($Editor) {
  $ErrorActionPreference = 'Continue' # "no daemon running" while shutting down is fine
  $h = $Editor.Process.MainWindowHandle
  [UiT]::SetWindowPos($h, [IntPtr](-2), 0, 0, 0, 0, 3) | Out-Null   # no longer topmost
  $Editor.Process.CloseMainWindow() | Out-Null
  Start-Sleep -Seconds 2
  if (-not $Editor.Process.HasExited) { $Editor.Process | Stop-Process -Force }
  Invoke-Attome $Editor daemon stop 2>&1 | Out-Null
}

Export-ModuleMember -Function Start-Editor, Show-Window, Save-Shot, Move-Mouse, Click-At, Move-Drag, Release-Mouse, Scroll-At, Send-Key, Invoke-Attome, Stop-Editor
