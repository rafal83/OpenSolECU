param([Parameter(ValueFromRemainingArguments=$true)][string[]]$IdfArgs)
$ErrorActionPreference = 'Stop'
if (-not $env:IDF_PATH) {
    $idfCommand = Get-Command idf.py -ErrorAction SilentlyContinue
    if (-not $idfCommand) { throw 'Install ESP-IDF 5.5.1 and run export.ps1 first.' }
    $openSolIdfRoot = Split-Path (Split-Path $idfCommand.Source)
    $ErrorActionPreference = 'Continue'
    . (Join-Path $openSolIdfRoot 'export.ps1')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
$ErrorActionPreference = 'Continue'
python "$env:IDF_PATH/tools/idf.py" @IdfArgs
exit $LASTEXITCODE
