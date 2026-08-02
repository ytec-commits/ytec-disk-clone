param(
    [Parameter(Mandatory)]
    [string]$PackageRoot,

    [Parameter(Mandatory)]
    [string]$ZipPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-RegularNonReparseFile {
    param(
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description が見つかりません: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description のreparse pointは使用しません: $Path"
    }
    if ($item.Length -le 0) {
        throw "$Description が空です: $Path"
    }
}

$root = [IO.Path]::GetFullPath($PackageRoot)
$zip = [IO.Path]::GetFullPath($ZipPath)
if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    throw "ポータブル配布フォルダーが見つかりません: $root"
}
$rootItem = Get-Item -LiteralPath $root -Force
if (($rootItem.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "配布フォルダーのreparse pointは使用しません: $root"
}
Assert-RegularNonReparseFile -Path $zip -Description 'ポータブルZIP'

$expectedFiles = @(
    'ytec-tsumugi-drive.exe',
    'tools\New-WinPEAppValidationMedia.ps1',
    'tools\ytec-winpe-environment.exe',
    'winpe\ytec-winpe-app.exe',
    'winpe\ytec-winpe-gui.exe',
    'はじめに.txt',
    '操作ガイド.txt',
    '安全上の注意と既知の制限.txt',
    'プライバシーと通信.txt',
    'セキュリティ報告.txt',
    'THIRD-PARTY-NOTICES.txt',
    'SBOM.spdx.json',
    'licenses\README.md',
    'licenses\LINE-Seed-JP-OFL-1.1.txt',
    'SHA256SUMS.txt')
$actualFiles = @(
    Get-ChildItem -LiteralPath $root -Recurse -File |
        ForEach-Object {
            $_.FullName.Substring($root.Length).TrimStart('\')
        } |
        Sort-Object)
if ($actualFiles.Count -ne $expectedFiles.Count) {
    throw "配布フォルダーのファイル数が不正です: $($actualFiles.Count)"
}
foreach ($relative in $expectedFiles) {
    if ($actualFiles -notcontains $relative) {
        throw "配布フォルダーの必須ファイルがありません: $relative"
    }
    Assert-RegularNonReparseFile `
        -Path (Join-Path $root $relative) `
        -Description $relative
}

$forbiddenExtensions = @(
    '.wim', '.iso', '.cab', '.msi', '.msix', '.vhd', '.vhdx')
$forbiddenNames = @(
    'dism.exe', 'oscdimg.exe', 'mbr2gpt.exe', 'bcdboot.exe',
    'diskpart.exe', 'copype.cmd', 'makewinpemedia.cmd')
foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -File) {
    if ($forbiddenExtensions -contains $file.Extension.ToLowerInvariant() -or
        $forbiddenNames -contains $file.Name.ToLowerInvariant()) {
        throw "配布禁止ファイルを検出しました: $($file.FullName)"
    }
    if (($file.Attributes -band
            [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "配布物にreparse pointがあります: $($file.FullName)"
    }
}

$hashPath = Join-Path $root 'SHA256SUMS.txt'
$hashBytes = [IO.File]::ReadAllBytes($hashPath)
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$hashText = $utf8.GetString($hashBytes)
if ($hashText -notmatch [regex]::Escape('*はじめに.txt')) {
    throw 'SHA256SUMS.txtが日本語ファイル名をUTF-8で保持していません。'
}
$hashLines = @($hashText -split '\r?\n' |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($hashLines.Count -ne ($expectedFiles.Count - 1)) {
    throw "SHA256SUMS.txtの行数が不正です: $($hashLines.Count)"
}
foreach ($line in $hashLines) {
    if ($line -notmatch '^([0-9A-F]{64}) \*(.+)$') {
        throw "SHA256SUMS.txtの行形式が不正です: $line"
    }
    $relative = $matches[2].Replace('/', '\')
    $filePath = Join-Path $root $relative
    Assert-RegularNonReparseFile -Path $filePath -Description $relative
    $actual = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash
    if ($actual -ne $matches[1]) {
        throw "配布ファイルのSHA-256が一致しません: $relative"
    }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $entries = @($archive.Entries |
        Where-Object { -not [string]::IsNullOrEmpty($_.Name) })
    if ($entries.Count -ne $expectedFiles.Count) {
        throw "ZIPのファイル数が不正です: $($entries.Count)"
    }
    foreach ($relative in $expectedFiles) {
        $entryName = $relative.Replace('\', '/')
        $entry = @($entries | Where-Object {
            $_.FullName.Equals(
                $entryName,
                [StringComparison]::Ordinal)
        })
        if ($entry.Count -ne 1 -or $entry[0].Length -le 0) {
            throw "ZIPの必須ファイルがありません: $entryName"
        }
    }
    foreach ($entry in $entries) {
        $extension = [IO.Path]::GetExtension(
            $entry.Name).ToLowerInvariant()
        if ($forbiddenExtensions -contains $extension -or
            $forbiddenNames -contains $entry.Name.ToLowerInvariant()) {
            throw "ZIPに配布禁止ファイルがあります: $($entry.FullName)"
        }
    }
} finally {
    $archive.Dispose()
}

$report = [ordered]@{
    schemaVersion = 1
    packageRoot = $root
    zipPath = $zip
    fileCount = $expectedFiles.Count
    hashedFileCount = $hashLines.Count
    zipSha256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    repositoryContainsMicrosoftPayload = $false
}
Write-Output ('TSUMUGI_PORTABLE_ARTIFACT_PASS=' +
    ($report | ConvertTo-Json -Depth 3 -Compress))
