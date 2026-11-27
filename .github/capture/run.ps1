param([string]$Game, [string]$Out)
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class U {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
New-Item -ItemType Directory -Force $Out | Out-Null
function Shot($name) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
  $bmp.Save("$Out\$name.png"); $g.Dispose(); $bmp.Dispose()
  Write-Host "shot $name $($b.Width)x$($b.Height)"
}
$p = Start-Process -FilePath "$Game\SunsetRacer2.exe" -WorkingDirectory $Game -PassThru
$keys = @{}
foreach ($line in Get-Content "$PSScriptRoot\script.txt") {
  $t = $line.Trim(); if ($t -eq '' -or $t.StartsWith('#')) { continue }
  $a = $t -split '\s+'
  if ($p.HasExited) { Write-Host "game exited code $($p.ExitCode)"; break }
  try { if ($p.MainWindowHandle -ne 0) { [U]::SetForegroundWindow($p.MainWindowHandle) | Out-Null } } catch {}
  switch ($a[0]) {
    'wait'  { Start-Sleep -Milliseconds ([double]$a[1] * 1000) }
    'shot'  { Shot $a[1] }
    'click' { [U]::SetCursorPos([int]$a[1], [int]$a[2]); Start-Sleep -Milliseconds 150; [U]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [U]::mouse_event(4,0,0,0,[UIntPtr]::Zero) }
    'move'  { [U]::SetCursorPos([int]$a[1], [int]$a[2]) }
    'down'  { [U]::keybd_event([Convert]::ToByte($a[1],16),0,0,[UIntPtr]::Zero) }
    'up'    { [U]::keybd_event([Convert]::ToByte($a[1],16),0,2,[UIntPtr]::Zero) }
    'key'   { $vk=[Convert]::ToByte($a[1],16); [U]::keybd_event($vk,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds ($(if($a.Count -gt 2){[int]$a[2]}else{80})); [U]::keybd_event($vk,0,2,[UIntPtr]::Zero) }
    'rec'   { Start-Process ffmpeg -ArgumentList "-loglevel error -y -f gdigrab -framerate 30 -t $($a[1]) -i desktop -c:v libx264 -preset ultrafast -crf 18 $Out\$($a[2]).mp4" -NoNewWindow }
    'shots' { for ($i=0; $i -lt [int]$a[1]; $i++) { Shot ("$($a[2])-{0:d2}" -f $i); Start-Sleep -Milliseconds ([int]$a[3]) } }
  }
}
Start-Sleep 2
Get-Process | Where-Object { $_.MainWindowTitle } | Format-Table Id, ProcessName, MainWindowTitle -AutoSize | Out-String | Write-Host
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
