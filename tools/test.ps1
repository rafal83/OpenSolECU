$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$compilerRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $compilerRoot) { throw 'Visual Studio C++ Build Tools required for host tests.' }
$env:PATH = (Split-Path $vswhere) + ';' + $env:PATH
$ErrorActionPreference = 'Continue'
& $env:ComSpec /c tools\test.cmd "$compilerRoot"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
python tests/check_pcapng.py
exit $LASTEXITCODE
