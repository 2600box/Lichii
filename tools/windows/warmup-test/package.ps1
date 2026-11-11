$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force package | Out-Null
Copy-Item build/release/bin/* package/ -Recurse
# Tests and symbols are evidence, not runtime dependencies.
Get-ChildItem package -File | Where-Object { $_.Name -match 'test.*\.exe$|\.pdb$|\.lib$|\.exp$' } | Remove-Item
foreach ($exe in 'lichii.exe', 'eden-cli.exe', 'lichii-updater.exe') {
    if (-not (Test-Path "package/$exe")) { throw "Missing $exe" }
}
windeployqt --release --no-translations --compiler-runtime package/lichii.exe
if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed' }
# A file keeps the empty portable directory in the artifact ZIP. Never copy personal data.
New-Item -ItemType Directory -Force package/user | Out-Null
Set-Content package/user/PORTABLE.txt 'This directory isolates this test from installed emulator profiles. Do not remove it.'
Set-Content package/README-TEST.txt @'
Build 150 + experimental graphics pipeline libraries and shader warm-up.
Extract into a NEW directory. Keep the user folder for isolated portable settings.
No game files, keys, firmware or prepared shaders are provided.
Add only your own files to this portable instance. Do not import an existing profile.
Graphics > Advanced: enable Shader libraries; choose Prepare shaders before the game starts.
Right-click your own game: Prepare shaders (warm-up). Shader libraries are experimental.
PGO and ThinLTO are compile-time options; this package has not been benchmarked on your GPU.
'@
Get-ChildItem package -Recurse -File | ForEach-Object {
    "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)  $([IO.Path]::GetRelativePath((Resolve-Path package), $_.FullName))"
} | Set-Content evidence/package-sha256.txt
