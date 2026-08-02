[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('CorruptImage', 'TamperedJob')]
    [string] $Scenario,

    [ValidateRange(30, 180)]
    [int] $BootWaitSeconds = 70,

    [ValidateRange(2, 20)]
    [int] $TimeoutMinutes = 10
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

& (Join-Path $PSScriptRoot 'Invoke-ProductJobCancellationVmTest.ps1') `
    -Profile Restore `
    -Scenario $Scenario `
    -BootWaitSeconds $BootWaitSeconds `
    -TimeoutMinutes $TimeoutMinutes
