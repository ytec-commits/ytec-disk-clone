$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$scriptPath = Join-Path $PSScriptRoot 'New-PortablePackage.ps1'

$tokens = $null
$parseErrors = $null
[Management.Automation.Language.Parser]::ParseFile(
    $scriptPath,
    [ref]$tokens,
    [ref]$parseErrors) | Out-Null
if ($parseErrors.Count -ne 0) {
    throw "Portable package script parse failed: $($parseErrors[0].Message)"
}

function Assert-Rejected {
    param(
        [Parameter(Mandatory)]
        [string]$OutputRoot,
        [Parameter(Mandatory)]
        [string]$ExpectedMessage
    )

    try {
        & $scriptPath -OutputRoot $OutputRoot
        throw "拒否されるべき出力先が許可されました: $OutputRoot"
    } catch {
        if ($_.Exception.Message -notlike "*$ExpectedMessage*") {
            throw "想定外の拒否理由です: $($_.Exception.Message)"
        }
    }
}

Assert-Rejected `
    -OutputRoot (Join-Path $repoRoot 'out\forbidden-portable') `
    -ExpectedMessage 'リポジトリ外'
Assert-Rejected `
    -OutputRoot ([IO.Path]::GetPathRoot($repoRoot)) `
    -ExpectedMessage 'ドライブ直下'
Assert-Rejected `
    -OutputRoot $env:TEMP `
    -ExpectedMessage '既存の出力先'

$preflightOutput = Join-Path $env:TEMP `
    ('Y-TEC-Tsumugi-Drive-portable-preflight-' +
        [guid]::NewGuid().ToString('N'))
$preflightZip = $preflightOutput + '.zip'
$result = & $scriptPath -OutputRoot $preflightOutput
if ($result -notlike 'TSUMUGI_PORTABLE_PREFLIGHT_PASS=*') {
    throw "事前検証の成功マーカーがありません: $result"
}
$preflight = $result.Substring(
    'TSUMUGI_PORTABLE_PREFLIGHT_PASS='.Length) | ConvertFrom-Json

if ($preflight.schemaVersion -ne 1 -or
    $preflight.product -ne 'Y-TEC Tsumugi Drive' -or
    $preflight.version -ne '0.2.0-dev' -or
    $preflight.buildRequested -ne $false -or
    $preflight.repositoryContainsMicrosoftPayload -ne $false) {
    throw 'ポータブル配布物の事前検証メタデータが不正です。'
}
if ($preflight.outputRoot -ne [IO.Path]::GetFullPath($preflightOutput) -or
    $preflight.zipPath -ne [IO.Path]::GetFullPath($preflightZip)) {
    throw 'フォルダーとZIPの非上書き出力先が分離されていません。'
}

$requiredFiles = @(
    'windowsApp',
    'environment',
    'winpeCli',
    'winpeGui',
    'builderScript',
    'readme',
    'operationGuide',
    'safetyAndLimitations',
    'privacyAndNetwork',
    'securityReporting',
    'notices',
    'sbom',
    'licenseReadme',
    'lineSeedLicense')
foreach ($name in $requiredFiles) {
    $file = $preflight.files.$name
    if ($null -eq $file -or
        $file.length -le 0 -or
        $file.sha256 -notmatch '^[0-9A-F]{64}$' -or
        -not (Test-Path -LiteralPath $file.path -PathType Leaf)) {
        throw "配布元ファイルの固定検証が不足しています: $name"
    }
}

$forbiddenExtensions = @(
    '.wim', '.iso', '.cab', '.msi', '.msix', '.vhd', '.vhdx')
foreach ($name in $requiredFiles) {
    $extension = [IO.Path]::GetExtension(
        [string]$preflight.files.$name.path).ToLowerInvariant()
    if ($forbiddenExtensions -contains $extension) {
        throw "事前検証へ配布禁止媒体が混入しました: $name"
    }
}

foreach ($path in @($preflightOutput, $preflightZip)) {
    if (Test-Path -LiteralPath $path) {
        throw "事前検証だけで出力が作成されました: $path"
    }
}

function Remove-ExactPortableTestArtifact {
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    $temporaryRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\')
    $candidate = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetDirectoryName($candidate).TrimEnd('\')
    $leaf = [IO.Path]::GetFileName($candidate)
    if (-not $parent.Equals(
            $temporaryRoot,
            [StringComparison]::OrdinalIgnoreCase) -or
        $leaf -notmatch
            '^Y-TEC-Tsumugi-Drive-package-ci-[0-9a-f]{32}(?:\.zip)?$') {
        throw "一時配布物の削除対象が固定境界外です: $candidate"
    }
    if (-not (Test-Path -LiteralPath $candidate)) {
        return
    }
    $item = Get-Item -LiteralPath $candidate -Force
    if (($item.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "一時配布物のreparse pointは削除しません: $candidate"
    }
    if ($item.PSIsContainer) {
        Remove-Item -LiteralPath $candidate -Recurse -Force
    } else {
        Remove-Item -LiteralPath $candidate -Force
    }
}

$artifactRoot = Join-Path $env:TEMP `
    ('Y-TEC-Tsumugi-Drive-package-ci-' +
        [guid]::NewGuid().ToString('N'))
$artifactZip = $artifactRoot + '.zip'
try {
    $packageResult = & $scriptPath `
        -OutputRoot $artifactRoot `
        -BuildPackage
    $packageMarker = @($packageResult |
        Where-Object {
            $_ -like 'TSUMUGI_PORTABLE_PACKAGE_PASS=*'
        } |
        Select-Object -Last 1)
    if ($packageMarker.Count -ne 1) {
        throw '実配布ZIPの作成成功マーカーがありません。'
    }
    $package = $packageMarker[0].Substring(
        'TSUMUGI_PORTABLE_PACKAGE_PASS='.Length) | ConvertFrom-Json
    if ($package.outputRoot -ne [IO.Path]::GetFullPath($artifactRoot) -or
        $package.zip.path -ne [IO.Path]::GetFullPath($artifactZip) -or
        $package.repositoryContainsMicrosoftPayload -ne $false) {
        throw '実配布ZIPの完成報告が要求した出力と一致しません。'
    }
    $artifactResult = & (Join-Path $PSScriptRoot `
        'Test-PortablePackageArtifact.ps1') `
        -PackageRoot $artifactRoot `
        -ZipPath $artifactZip
    if ($artifactResult -notlike
        'TSUMUGI_PORTABLE_ARTIFACT_PASS=*') {
        throw "実配布ZIPの監査成功マーカーがありません: $artifactResult"
    }
} finally {
    Remove-ExactPortableTestArtifact -Path $artifactZip
    Remove-ExactPortableTestArtifact -Path $artifactRoot
}

Write-Output 'Portable package boundary tests: PASS'
