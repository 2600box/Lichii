$ErrorActionPreference = 'Stop'
$smoke = Join-Path $env:RUNNER_TEMP 'lichii-native-smoke'
Copy-Item package $smoke -Recurse
# Exclude development SDKs and build tools from the process DLL search path.
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
$p = Start-Process (Join-Path $smoke 'lichii.exe') -WorkingDirectory $smoke -PassThru -RedirectStandardOutput evidence/native-stdout.txt -RedirectStandardError evidence/native-stderr.txt
try {
    Start-Sleep -Seconds 20
    $p.Refresh()
    if ($p.HasExited) { throw "Native GUI exited with code $($p.ExitCode)" }
    if ($p.MainWindowHandle -eq 0) { throw 'No native GUI window appeared' }
    "Native Windows GUI remained alive for 20 seconds with a visible window. Title: $($p.MainWindowTitle). PATH contains only Windows system directories; Qt/Vulkan SDKs were not installed by this job. No game was booted. GPU warm-up and performance remain unverified." | Set-Content evidence/native-runtime-smoke.txt
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id }
    Get-ChildItem (Join-Path $smoke 'user') -Recurse -File -Filter '*log*' | Copy-Item -Destination evidence -Force
}
