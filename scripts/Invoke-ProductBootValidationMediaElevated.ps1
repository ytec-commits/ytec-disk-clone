param(
    [Parameter(Mandatory)]
    [string] $BaseProductMediaRoot,

    [Parameter(Mandatory)]
    [string] $OutputRoot,

    [Parameter(Mandatory)]
    [string] $LogPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$builder = Join-Path $PSScriptRoot 'New-ProductBootValidationMedia.ps1'
$outputFullPath = [IO.Path]::GetFullPath($OutputRoot)
$logFullPath = [IO.Path]::GetFullPath($LogPath)
$logParent = Split-Path -Parent $logFullPath
if (-not (Test-Path -LiteralPath $logParent -PathType Container)) {
    throw "The log parent directory is missing: $logParent"
}
if (Test-Path -LiteralPath $outputFullPath) {
    throw "The validation media output root already exists: $outputFullPath"
}

$exitCode = 0
Start-Transcript -LiteralPath $logFullPath -Force | Out-Null
try {
    New-Item -ItemType Directory -Path $outputFullPath | Out-Null
    foreach ($profile in 'VssRestore','GptClone','MbrClone') {
        & $builder `
            -BaseProductMediaRoot $BaseProductMediaRoot `
            -OutputRoot (Join-Path $outputFullPath $profile) `
            -Profile $profile `
            -BuildMedia
    }
}
catch {
    $exitCode = 1
    Write-Error ($_ | Format-List * -Force | Out-String)
}
finally {
    Stop-Transcript | Out-Null
}
exit $exitCode
