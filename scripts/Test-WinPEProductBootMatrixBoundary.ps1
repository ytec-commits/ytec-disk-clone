$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptPath = Join-Path `
    $PSScriptRoot `
    'Invoke-WinPEProductBootMatrixVmTest.ps1'
$tokens = $null
$parseErrors = $null
[Management.Automation.Language.Parser]::ParseFile(
    $scriptPath,
    [ref]$tokens,
    [ref]$parseErrors) | Out-Null
if ($parseErrors.Count -ne 0) {
    throw "製品WinPE起動マトリクススクリプトを解析できません: $($parseErrors[0].Message)"
}

$source = Get-Content -LiteralPath $scriptPath -Raw
foreach ($requiredMarker in @(
        "ValidatePattern('^YDC-WinPE-Boot-Matrix$')",
        "-Arguments @('list', 'runningvms')",
        'Assert-NetworkDisabled',
        "controllerNames.Count -ne 1",
        'hardDiskCount = 0',
        'physicalDiskOrUsbUsed = $false',
        "'CAPTURED_PENDING_VISUAL_INSPECTION'",
        'originalFirmware',
        'originalSecureBoot',
        'originalMedium',
        'runningVmCountAfter')) {
    if (-not $source.Contains($requiredMarker)) {
        throw "起動マトリクスの安全条件がありません: $requiredMarker"
    }
}

foreach ($forbiddenPattern in @(
        '(?i)\bunregistervm\b',
        '(?i)\bregistervm\b',
        '(?i)\bclosemedium\b',
        '(?i)\bcreatemedium\b',
        '(?i)\brawdisk\b',
        '(?i)\bhostdrive\b',
        '(?i)\busbattach\b',
        '(?i)\bsnapshot\s+delete\b',
        '(?i)\bRemove-Item\b')) {
    if ($source -match $forbiddenPattern) {
        throw "起動マトリクスに禁止操作があります: $forbiddenPattern"
    }
}

$mediaBuilderPath = Join-Path `
    $PSScriptRoot `
    'New-ProductBootValidationMedia.ps1'
$builderTokens = $null
$builderParseErrors = $null
[Management.Automation.Language.Parser]::ParseFile(
    $mediaBuilderPath,
    [ref]$builderTokens,
    [ref]$builderParseErrors) | Out-Null
if ($builderParseErrors.Count -ne 0) {
    throw "製品起動検証ISO生成スクリプトを解析できません: $($builderParseErrors[0].Message)"
}
$builderSource = Get-Content -LiteralPath $mediaBuilderPath -Raw
foreach ($requiredMarker in @(
        'Assert-ExternalNewOutputPath',
        'The verified base media must remain outside the repository.',
        "repositoryContainsMicrosoftPayload -ne `$false",
        "certificateGeneration -ne '2023CA'",
        "adkVersion -ne '10.1.26100.2454'",
        "dismVersion -ne '10.0.26100.8972'",
        "servicingUpdate -ne 'KB5101684'",
        'Assert-MicrosoftSignature -Path $dism',
        'Assert-MicrosoftSignature -Path $oscdimg',
        "physicalDiskOrUsbPermitted = `$false",
        "repositoryContainsMicrosoftPayload = `$false",
        "vmOnly = `$true",
        'Administrator rights are required to mount and commit boot.wim.',
        "'/Commit', '/CheckIntegrity'")) {
    if (-not $builderSource.Contains($requiredMarker)) {
        throw "製品起動検証ISO生成の安全条件がありません: $requiredMarker"
    }
}

$productBootScripts = @(
    [ordered]@{
        Name = 'GPT製品クローン単独起動'
        Path = Join-Path $PSScriptRoot 'Invoke-ProductGptCloneBootVmTest.ps1'
        TargetMarker = 'targetOnlyUefiSecureBootVerified = $targetBootVerified'
        ProtectedMarker = 'protectedSourceVdiUnchanged = $protectedHashBefore -eq $protectedHashAfter'
        ResultGate = '$winpeShutdownObserved -and $targetBootVerified -and'
        ProbeTransport = 'GuestControl'
        GuestUserMarker = '$guestUser = ''YbcTest'''
        PasswordFileMarker = 'YWB-Win10-22H2-x64-Clean.password.txt'
    },
    [ordered]@{
        Name = 'MBR製品クローン単独起動'
        Path = Join-Path $PSScriptRoot 'Invoke-ProductMbrCloneBootVmTest.ps1'
        TargetMarker = 'targetOnlyLegacyBiosVerified = $targetBootVerified'
        ProtectedMarker = 'protectedSourceVdiUnchanged = $protectedHashBefore -eq $protectedHashAfter'
        ResultGate = '$winpeShutdownObserved -and $targetBootVerified -and'
        ProbeTransport = 'KeyboardUart'
    },
    [ordered]@{
        Name = 'VSS製品復元単独起動'
        Path = Join-Path $PSScriptRoot 'Invoke-ProductVssRestoreVmTest.ps1'
        TargetMarker = 'targetOnlyUefiSecureBootVerified = $targetBootVerified'
        ProtectedMarker = 'protectedImageVdiUnchanged = $protectedHashBefore -eq $protectedHashAfter'
        ResultGate = '$targetBootVerified -and'
        ProbeTransport = 'GuestControl'
        GuestUserMarker = '$guestUser = ''YbcTest'''
        PasswordFileMarker = 'YWB-Win10-22H2-x64-Clean.password.txt'
    }
)
foreach ($item in $productBootScripts) {
    $itemTokens = $null
    $itemParseErrors = $null
    [Management.Automation.Language.Parser]::ParseFile(
        $item.Path,
        [ref]$itemTokens,
        [ref]$itemParseErrors) | Out-Null
    if ($itemParseErrors.Count -ne 0) {
        throw "$($item.Name)スクリプトを解析できません: $($itemParseErrors[0].Message)"
    }
    $itemSource = Get-Content -LiteralPath $item.Path -Raw
    foreach ($requiredMarker in @(
            "-Arguments @('list', 'runningvms')",
            'Assert-NetworkDisabled',
            'Get-FileHash -Algorithm SHA256',
            $item.TargetMarker,
            $item.ProtectedMarker,
            $item.ResultGate,
            'physicalDiskOrUsbUsed = $false',
            'nicDisabled = $true',
            'workerConfigurationRestored = $restoredConfiguration',
            'ShutdownScheduled=1')) {
        if (-not $itemSource.Contains($requiredMarker)) {
            throw "$($item.Name)の安全条件がありません: $requiredMarker"
        }
    }
    foreach ($forbiddenPattern in @(
            '(?i)\bunregistervm\b',
            '(?i)\bregistervm\b',
            '(?i)\bclosemedium\b',
            '(?i)\brawdisk\b',
            '(?i)\bhostdrive\b',
            '(?i)\busbattach\b',
            '(?i)\bsnapshot\s+delete\b',
            '(?i)\bguestcontrol\b[^\r\n]*\bcopyto\b',
            '(?i)\bguestcontrol\b[^\r\n]*\bcopyfrom\b',
            '(?i)\bguestcontrol\b[^\r\n]*\bmkdir\b',
            '(?i)\bguestcontrol\b[^\r\n]*\brm\b',
            '(?i)--password(?!file)(?:=|\s)',
            '(?is)\bGet-Content\b.{0,160}\$passwordFile',
            '(?is)\bReadAllText\b.{0,160}\$passwordFile')) {
        if ($itemSource -match $forbiddenPattern) {
            throw "$($item.Name)に禁止操作があります: $forbiddenPattern"
        }
    }
    if ($item.ProbeTransport -eq 'GuestControl') {
        foreach ($requiredMarker in @(
                $item.GuestUserMarker,
                $item.PasswordFileMarker,
                '''guestcontrol'', $vmName, ''stat''',
                '''guestcontrol'', $vmName, ''run''',
                '"--passwordfile=$passwordFile"',
                '''-EncodedCommand''',
                'YDC_TARGET_PROBE_LAUNCHED',
                'targetProbeTransport = ''VirtualBox GuestControl with fixed password file''')) {
            if (-not $itemSource.Contains($requiredMarker)) {
                throw "$($item.Name)のGuestControl安全条件がありません: $requiredMarker"
            }
        }
        foreach ($forbiddenPattern in @(
                '(?i)\bkeyboardputstring\b',
                '(?i)\bInstall-AndLaunchTargetValidation\b')) {
            if ($itemSource -match $forbiddenPattern) {
                throw "$($item.Name)に禁止されたキー入力プローブがあります: $forbiddenPattern"
            }
        }
    }
    else {
        foreach ($requiredMarker in @(
                'function Invoke-KeyboardRunCommand',
                "Matches(`$Command, '.{1,48}')",
                "Matches(`$HexEncodedScript, '.{1,160}')",
                'certutil.exe -f -decodehex YdcValidation.hex',
                'YdcValidation.ps1 12',
                'YDCUART=',
                'targetProbeTransport = ''VirtualBox keyboard staging with UART result''')) {
            if (-not $itemSource.Contains($requiredMarker)) {
                throw "$($item.Name)のUARTプローブ安全条件がありません: $requiredMarker"
            }
        }
        foreach ($forbiddenPattern in @(
                '(?i)\bguestcontrol\b',
                '(?i)\bpasswordfile\b')) {
            if ($itemSource -match $forbiddenPattern) {
                throw "$($item.Name)はGuest Additions資格情報へ依存できません: $forbiddenPattern"
            }
        }
    }
}

Write-Output 'WinPE product boot matrix boundary tests: PASS'
