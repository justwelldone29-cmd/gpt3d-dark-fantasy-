$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
& python (Join-Path $root 'prepare.py')
if ($LASTEXITCODE -ne 0) { throw 'Source preparation failed' }
$generated = Join-Path $root 'generated'
Push-Location (Join-Path $generated 'internal')
try {
    & cmd.exe /d /c build.bat
    if ($LASTEXITCODE -ne 0) { throw 'Native DLL build failed' }
} finally { Pop-Location }
Push-Location (Join-Path $generated 'runtime')
try {
    & python -m unittest discover -s tests -v
    if ($LASTEXITCODE -ne 0) { throw 'Launcher tests failed' }
    & node tests/aim_math.test.js
    if ($LASTEXITCODE -ne 0) { throw 'Target selection tests failed' }
    & node tests/agent_guards.test.js
    if ($LASTEXITCODE -ne 0) { throw 'Runtime guard tests failed' }
    & node --check agent.js
    if ($LASTEXITCODE -ne 0) { throw 'Agent syntax check failed' }
    & python -m PyInstaller --noconfirm --clean --onefile --console --name RobRunLauncher --add-data 'aim_math.js;.' --add-data 'agent.js;.' --collect-all frida run.py
    if ($LASTEXITCODE -ne 0) { throw 'Launcher build failed' }
} finally { Pop-Location }
$package = Join-Path $root 'package'
New-Item -ItemType Directory -Force -Path $package | Out-Null
Copy-Item (Join-Path $generated 'internal/RobRunInternal.dll') $package -Force
Copy-Item (Join-Path $generated 'runtime/dist/RobRunLauncher.exe') $package -Force
Copy-Item (Join-Path $root 'README-USER.txt') (Join-Path $package 'README.txt') -Force
& (Join-Path $package 'RobRunLauncher.exe') --help
if ($LASTEXITCODE -ne 0) { throw 'Frozen launcher smoke test failed' }
Get-ChildItem $package -File | Get-FileHash -Algorithm SHA256 | Format-Table -AutoSize
Compress-Archive -Path (Join-Path $package '*') -DestinationPath (Join-Path $root 'RobRun-Windows-x64.zip') -Force
