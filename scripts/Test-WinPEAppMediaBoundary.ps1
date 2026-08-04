$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$scriptPath = Join-Path $PSScriptRoot 'New-WinPEAppValidationMedia.ps1'
$elevatedWrapper = Join-Path $PSScriptRoot 'Invoke-WinPEAppMediaElevated.ps1'

foreach ($file in @($scriptPath, $elevatedWrapper)) {
    $tokens = $null
    $parseErrors = $null
    [Management.Automation.Language.Parser]::ParseFile(
        $file,
        [ref]$tokens,
        [ref]$parseErrors) | Out-Null
    if ($parseErrors.Count -ne 0) {
        throw "WinPEApp media script parse failed: $($parseErrors[0].Message)"
    }
    if ((Get-Content -LiteralPath $file -Raw) -match
        '(?i)\butf8NoBOM\b') {
        throw "Windows PowerShell 5.1非互換のencoding指定があります: $file"
    }
}

$builderSource = Get-Content -LiteralPath $scriptPath -Raw
foreach ($requiredSafetyMarker in @(
        '0x80000008',
        'sourceWasWimProjection',
        'microsoftSignatureVerified',
        "subsystem = 'EFI Application'",
        'sourceHashBefore -ne $sourceHashAfter')) {
    if (-not $builderSource.Contains($requiredSafetyMarker)) {
        throw "WIM投影ファイル検証の安全条件がありません: $requiredSafetyMarker"
    }
}

foreach ($requiredUsbInitializationMarker in @(
        'function Get-UsbPartitionsAllowEmpty',
        'CmdletizationQuery_NotFound_DiskNumber,Get-Partition',
        'function Initialize-VerifiedUsbTarget',
        '-AllowUnpartitioned',
        'Clear-Disk',
        '-InputObject $before.disk',
        'Update-Disk',
        'Initialize-Disk',
        'Set-Disk',
        '-PartitionStyle MBR',
        'New-Partition',
        '$maximumFat32Bytes = [UInt64](30GB)',
        'Format-Volume',
        '-FileSystem FAT32',
        '-RequireMbr')) {
    if (-not $builderSource.Contains($requiredUsbInitializationMarker)) {
        throw "対象限定USB自動初期化の安全条件がありません: $requiredUsbInitializationMarker"
    }
}
foreach ($singleUsbWriter in @(
        'Clear-Disk',
        'Initialize-Disk',
        'Set-Disk',
        'Format-Volume')) {
    if ([regex]::Matches(
            $builderSource,
            "\b${singleUsbWriter}\b").Count -ne 1) {
        throw "USB自動初期化の${singleUsbWriter}は監査済み1箇所だけに制限します。"
    }
}

$builderTokens = $null
$builderParseErrors = $null
$builderAst = [Management.Automation.Language.Parser]::ParseFile(
    $scriptPath,
    [ref]$builderTokens,
    [ref]$builderParseErrors)
$partitionHelperAst = $builderAst.Find(
    {
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Get-UsbPartitionsAllowEmpty'
    },
    $true)
if ($null -eq $partitionHelperAst) {
    throw '空USBパーティション照会ヘルパーを抽出できません。'
}
Invoke-Expression $partitionHelperAst.Extent.Text

$script:verifiedEmptyUsbCount = 0
function Get-VerifiedUsbDisk {
    param(
        [int]$DiskNumber,
        [UInt64]$SizeBytes,
        [AllowEmptyString()]
        [string]$SerialSuffix,
        [string]$DeviceInstanceId
    )

    $script:verifiedEmptyUsbCount++
    return [ordered]@{ diskNumber = 3 }
}
function Get-Partition {
    [CmdletBinding()]
    param([int]$DiskNumber)

    $record = [Management.Automation.ErrorRecord]::new(
        [InvalidOperationException]::new('no partitions'),
        'CmdletizationQuery_NotFound_DiskNumber',
        [Management.Automation.ErrorCategory]::ObjectNotFound,
        $DiskNumber)
    $PSCmdlet.ThrowTerminatingError($record)
}
$emptyPartitions = @(
    Get-UsbPartitionsAllowEmpty `
        -DiskNumber 3 `
        -SizeBytes 62136188928 `
        -SerialSuffix '050490C0' `
        -DeviceInstanceId 'USB\MOCK'
)
if ($emptyPartitions.Count -ne 0 -or
    $script:verifiedEmptyUsbCount -ne 1) {
    throw 'パーティション0件のObjectNotFoundを安全に空として扱えません。'
}

function Get-Partition {
    [CmdletBinding()]
    param([int]$DiskNumber)

    $record = [Management.Automation.ErrorRecord]::new(
        [InvalidOperationException]::new('provider failure'),
        'UnexpectedProviderFailure',
        [Management.Automation.ErrorCategory]::InvalidOperation,
        $DiskNumber)
    $PSCmdlet.ThrowTerminatingError($record)
}
try {
    Get-UsbPartitionsAllowEmpty `
        -DiskNumber 3 `
        -SizeBytes 62136188928 `
        -SerialSuffix '050490C0' `
        -DeviceInstanceId 'USB\MOCK' | Out-Null
    throw '想定外のパーティション照会エラーが許可されました。'
} catch {
    if ($_.FullyQualifiedErrorId -notlike
        'UnexpectedProviderFailure,Get-Partition*') {
        throw
    }
}
Remove-Item Function:\Get-Partition
Remove-Item Function:\Get-VerifiedUsbDisk
Remove-Item Function:\Get-UsbPartitionsAllowEmpty

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
    -OutputRoot (Join-Path $repoRoot 'out\forbidden-winpe') `
    -ExpectedMessage 'リポジトリ外'
Assert-Rejected `
    -OutputRoot ([IO.Path]::GetPathRoot($repoRoot)) `
    -ExpectedMessage 'ドライブ直下'
Assert-Rejected `
    -OutputRoot $env:TEMP `
    -ExpectedMessage '既存の出力先'

$preflightOutput = Join-Path $env:LOCALAPPDATA `
    ('YTEC\ytec-disk-clone\preflight-only\' + [guid]::NewGuid().ToString('N'))
$result = & $scriptPath -OutputRoot $preflightOutput
if ($result -notlike 'WINPE_APP_MEDIA_PREFLIGHT_PASS=*') {
    throw "事前検証の成功マーカーがありません: $result"
}
$preflight = $result.Substring(
    'WINPE_APP_MEDIA_PREFLIGHT_PASS='.Length) | ConvertFrom-Json
if ($preflight.winpeGui.machine -ne 'AMD64' -or
    $preflight.winpeGui.optionalHeader -ne 'PE32+' -or
    $preflight.winpeGui.sha256 -notmatch '^[0-9A-F]{64}$') {
    throw 'WinPE GUIのAMD64形式またはSHA-256事前検証が不足しています。'
}
foreach ($required in @('GDI32.dll', 'USER32.dll')) {
    if ($preflight.winpeGui.dependentDlls -notcontains $required) {
        throw "WinPE GUIの必須DLLが固定検査に含まれていません: $required"
    }
}
if ($preflight.winpeGui.dynamicallyLoadedSystemDlls -notcontains
    'comdlg32.dll') {
    throw 'WinPE GUIの動的System32 DLLがmanifestへ記録されていません。'
}
if ($preflight.japaneseFontSupport.repositoryCopy -ne $false -or
    $preflight.japaneseFontSupport.path -notlike
        '*\WinPE-FontSupport-JA-JP.cab' -or
    $preflight.japaneseFontSupport.sha256 -notmatch '^[0-9A-F]{64}$') {
    throw 'ローカルADK日本語フォントの非同梱境界またはSHA-256記録が不正です。'
}
if ($preflight.lineSeedLicense.name -ne 'LINE Seed JP' -or
    $preflight.lineSeedLicense.version -ne 'LINESeedJP_20241105' -or
    $preflight.lineSeedLicense.license -ne 'OFL-1.1' -or
    $preflight.lineSeedLicense.sha256 -notmatch '^[0-9A-F]{64}$') {
    throw 'LINE Seed JPの版・OFL・SHA-256記録が不正です。'
}
if (Test-Path -LiteralPath $preflightOutput) {
    throw '事前検証だけで出力先が作成されました。'
}

$explicitOutput = Join-Path $env:LOCALAPPDATA `
    ('YTEC\ytec-disk-clone\product-preflight\' +
        [guid]::NewGuid().ToString('N'))
$explicitIso = Join-Path $env:TEMP `
    ('Y-TEC-Tsumugi-Drive-' + [guid]::NewGuid().ToString('N') + '.iso')
$explicitResult = & $scriptPath `
    -OutputRoot $explicitOutput `
    -FinalIsoPath $explicitIso `
    -DiagnosticPath (Join-Path $repoRoot `
        'out\build\msvc-x64\src\MediaBuilder\ytec-winpe-environment.exe') `
    -WinPEAppPath (Join-Path $repoRoot `
        'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-app.exe') `
    -WinPEGuiPath (Join-Path $repoRoot `
        'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-gui.exe')
if ($explicitResult -notlike 'WINPE_APP_MEDIA_PREFLIGHT_PASS=*') {
    throw "製品配置事前検証の成功マーカーがありません: $explicitResult"
}
$explicitPreflight = $explicitResult.Substring(
    'WINPE_APP_MEDIA_PREFLIGHT_PASS='.Length) | ConvertFrom-Json
if ($explicitPreflight.outputRoot -ne
        [IO.Path]::GetFullPath($explicitOutput) -or
    $explicitPreflight.finalIsoPath -ne
        [IO.Path]::GetFullPath($explicitIso) -or
    $explicitPreflight.finalManifestPath -ne
        ([IO.Path]::GetFullPath($explicitIso) + '.manifest.json')) {
    throw '製品配置の完成ISO／manifest／一時作業先が分離されていません。'
}
foreach ($path in @(
        $explicitOutput,
        $explicitIso,
        ($explicitIso + '.manifest.json'))) {
    if (Test-Path -LiteralPath $path) {
        throw "製品配置の事前検証だけで出力が作成されました: $path"
    }
}

$usbPreflightOutput = Join-Path $env:LOCALAPPDATA `
    ('YTEC\ytec-disk-clone\usb-preflight-only\' +
        [guid]::NewGuid().ToString('N'))
$usbPreflightResult = & $scriptPath `
    -OutputRoot $usbPreflightOutput `
    -TargetUsbDrive 'Z:' `
    -ExpectedUsbDiskNumber 2147483000 `
    -ExpectedUsbSizeBytes 34359738368 `
    -ExpectedUsbSerialSuffix 'FAKE1234' `
    -ExpectedUsbDeviceInstanceId 'USB\VID_FAKE&PID_TEST\BOUNDARY' `
    -BuildUsb
if ($usbPreflightResult -notlike
        'WINPE_APP_MEDIA_PREFLIGHT_PASS=*') {
    throw "USB事前検証の成功マーカーがありません: $usbPreflightResult"
}
$usbPreflight = $usbPreflightResult.Substring(
    'WINPE_APP_MEDIA_PREFLIGHT_PASS='.Length) | ConvertFrom-Json
if (-not $usbPreflight.buildUsbRequested -or
    $usbPreflight.targetUsbDrive -ne 'Z:' -or
    $usbPreflight.expectedUsbDiskNumber -ne 2147483000) {
    throw 'USB事前検証が対象限定情報を正しく記録していません。'
}
if (Test-Path -LiteralPath $usbPreflightOutput) {
    throw 'USB事前検証だけで作業先またはUSBへの書込みが始まりました。'
}

try {
    & $scriptPath `
        -OutputRoot $usbPreflightOutput `
        -TargetUsbDrive 'Z:' `
        -ExpectedUsbDiskNumber 7 `
        -ExpectedUsbSizeBytes 34359738368 `
        -ExpectedUsbDeviceInstanceId 'USB\FAKE' | Out-Null
    throw 'BuildUsbなしのUSB対象情報が許可されました。'
} catch {
    if ($_.Exception.Message -notlike '*-BuildUsb*') {
        throw "BuildUsb境界の想定外エラーです: $($_.Exception.Message)"
    }
}

Write-Output 'WinPEApp media boundary tests: PASS'
