param(
    [Parameter(Mandatory)][string] $BaseWorkRoot,
    [Parameter(Mandatory)][string] $OutputRoot,
    [Parameter(Mandatory)][string] $LogPath,
    [switch] $MsrDiagnostic
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$exitCode = 0
Start-Transcript -LiteralPath $LogPath -Force | Out-Null
try {
    & (Join-Path $PSScriptRoot 'New-ProductMbr2GptValidationMedia.ps1') `
        -BaseWorkRoot $BaseWorkRoot `
        -OutputRoot $OutputRoot `
        -CertificateGeneration 2023CA `
        -MsrDiagnostic:$MsrDiagnostic
}
catch {
    $exitCode = 1
    Write-Error ($_ | Format-List * -Force | Out-String)
}
finally {
    Stop-Transcript | Out-Null
}
exit $exitCode
