$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $repoRoot 'third_party\dependencies.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

$allowedLicenses = @(
    'MIT'
    'BSD-2-Clause'
    'BSD-3-Clause'
    'Apache-2.0'
    'OFL-1.1'
)
$forbiddenPattern = '(?i)(^|[^A-Za-z])(AGPL|GPL|LGPL|SSPL|MPL|EPL|Commons Clause|BSL|Business Source License|NOASSERTION|UNKNOWN)([^A-Za-z]|$)'
$failures = @()

foreach ($dependency in $manifest.dependencies) {
    if ([string]::IsNullOrWhiteSpace($dependency.name) -or
        [string]::IsNullOrWhiteSpace($dependency.version) -or
        [string]::IsNullOrWhiteSpace($dependency.source) -or
        [string]::IsNullOrWhiteSpace($dependency.license)) {
        $failures += '依存台帳に必須項目がないエントリがあります。'
        continue
    }

    if ($dependency.license -match $forbiddenPattern -or
        $allowedLicenses -notcontains $dependency.license) {
        $failures += "未承認または禁止ライセンス: $($dependency.name) $($dependency.license)"
    }

    if ($dependency.approved -ne $true) {
        $failures += "人間の承認記録がありません: $($dependency.name)"
    }

    if ($dependency.license -eq 'OFL-1.1' -and
        $dependency.primaryPackagePurpose -ne 'FONT') {
        $failures += "OFL依存をFONTとして記録していません: $($dependency.name)"
    }

    $licensePath = Join-Path $repoRoot $dependency.licenseFile
    if ([string]::IsNullOrWhiteSpace($dependency.licenseFile) -or
        -not (Test-Path -LiteralPath $licensePath)) {
        $failures += "ライセンス本文がありません: $($dependency.name)"
    }
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    throw 'License check: FAIL'
}

Write-Output "License check: PASS ($($manifest.dependencies.Count) external dependencies)"
