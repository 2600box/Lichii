$ErrorActionPreference = 'Stop'
# Test a copy so no runtime-generated profile/cache files enter the deliverable.
$smoke = Join-Path $env:RUNNER_TEMP 'lichii-smoke'
Copy-Item package $smoke -Recurse
$env:QT_QPA_PLATFORM = 'offscreen'
$p = Start-Process (Join-Path $smoke 'lichii.exe') -WorkingDirectory $smoke -PassThru -RedirectStandardOutput evidence/gui-stdout.txt -RedirectStandardError evidence/gui-stderr.txt
Start-Sleep -Seconds 20
$p.Refresh()
if ($p.HasExited) { throw "GUI exited early with code $($p.ExitCode); inspect smoke logs" }
Set-Content evidence/runtime-smoke.txt "Packaged lichii.exe PID $($p.Id) remained alive for 20 seconds with Qt offscreen and a fresh portable user directory. No game booted; Vulkan pipeline creation and NVIDIA/Intel performance unverified."
Stop-Process -Id $p.Id
Get-ChildItem (Join-Path $smoke 'user') -Recurse -File -Filter '*log*' | Copy-Item -Destination evidence -Force
