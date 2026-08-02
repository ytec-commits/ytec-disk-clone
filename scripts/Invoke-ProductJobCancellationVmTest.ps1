[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Clone', 'Restore')]
    [string] $Profile,

    [ValidateSet(
        'Success',
        'Cancellation',
        'AutoOnce',
        'CorruptImage',
        'TamperedJob'
    )]
    [string] $Scenario = 'Cancellation',

    [ValidateRange(30, 180)]
    [int] $BootWaitSeconds = 70,

    [ValidateRange(2, 20)]
    [int] $TimeoutMinutes = 10,

    [switch] $AllowPendingProductIsoRefresh
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot '..\..'))
$labRoot = Join-Path $workspaceRoot 'business-apps\ytec-windows-backup'
$labVmRoot = Join-Path $labRoot '.validation\vms'
$vboxManage = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$cloneVm = 'YDC-WinPE-Product-Preflight'
$restoreVm = 'YDC-Standalone-BootRepair-BIOS-x64'
$vmName = if ($Profile -eq 'Clone') { $cloneVm } else { $restoreVm }
$profileArgument = $Profile.ToLowerInvariant()
$scenarioArgument = switch ($Scenario) {
    'Success' { 'success' }
    'Cancellation' { 'cancellation' }
    'AutoOnce' { 'auto-once' }
    'CorruptImage' { 'corrupt-image' }
    'TamperedJob' { 'tampered-job' }
}
if ($Scenario -in @('CorruptImage', 'TamperedJob') -and
    $Profile -ne 'Restore') {
    throw 'CorruptImageとTamperedJobはRestoreプロファイル専用です。'
}
if ($Scenario -eq 'AutoOnce' -and $Profile -ne 'Clone') {
    throw 'AutoOnceはCloneプロファイル専用です。'
}
$expectedMarker = if ($Scenario -eq 'AutoOnce') {
    'YDC_PRODUCT_AUTO_ONCE_PASS'
}
elseif ($Scenario -eq 'CorruptImage') {
    'YDC_PRODUCT_RESTORE_CORRUPT_IMAGE_PASS'
}
elseif ($Scenario -eq 'Success' -and $Profile -eq 'Clone') {
    'YDC_PRODUCT_CLONE_SUCCESS_PASS'
}
elseif ($Scenario -eq 'Success') {
    'YDC_PRODUCT_RESTORE_SUCCESS_PASS'
}
elseif ($Scenario -eq 'TamperedJob') {
    'YDC_PRODUCT_RESTORE_TAMPERED_JOB_PASS'
}
elseif ($Profile -eq 'Clone') {
    'YDC_PRODUCT_CLONE_CANCEL_PASS'
}
else {
    'YDC_PRODUCT_RESTORE_CANCEL_PASS'
}
$expectedFirmware = if ($Profile -eq 'Clone') { 'EFI64' } else { 'BIOS' }
$targetSizeMb = if ($Profile -eq 'Clone') { 3072 } else { 8 }
$targetFileName = if ($Scenario -eq 'AutoOnce') {
    'YDC-Product-Auto-Once-Target-3GiB.vdi'
}
elseif ($Profile -eq 'Clone' -and $Scenario -eq 'Success') {
    'YDC-Product-Clone-Success-Target-3GiB.vdi'
}
elseif ($Profile -eq 'Clone') {
    'YDC-Product-Clone-Cancel-Target-3GiB.vdi'
}
elseif ($Scenario -eq 'Success') {
    'YDC-Product-Restore-Success-Target-8MiB.vdi'
}
elseif ($Scenario -eq 'CorruptImage') {
    'YDC-Product-Restore-Corrupt-Image-Target-8MiB.vdi'
}
elseif ($Scenario -eq 'TamperedJob') {
    'YDC-Product-Restore-Tampered-Job-Target-8MiB.vdi'
}
else {
    'YDC-Product-Restore-Cancel-Target-8MiB.vdi'
}
$fixture = Join-Path $repoRoot `
    'out\build\msvc-x64-vm-destructive\tests\ytec-product-job-fixture-vm.exe'
$productGui = Join-Path $repoRoot `
    'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-gui.exe'
$operationHarness = if ($Scenario -eq 'AutoOnce') {
    Join-Path $repoRoot `
        'out\build\msvc-x64-vm-destructive\tests\ytec-product-auto-once-monitor-vm.exe'
}
elseif ($Scenario -in @('Success', 'Cancellation')) {
    Join-Path $repoRoot `
        'out\build\msvc-x64-vm-destructive\tests\ytec-product-job-cancellation-vm.exe'
}
else {
    Join-Path $repoRoot `
        'out\build\msvc-x64-vm-destructive\tests\ytec-product-job-failure-vm.exe'
}
$guestRunner = if ($Scenario -in @('Success', 'Cancellation', 'AutoOnce')) {
    Join-Path $repoRoot 'scripts\vm\Run-ProductJobCancellationValidation.cmd'
}
else {
    Join-Path $repoRoot 'scripts\vm\Run-ProductJobFailureValidation.cmd'
}
$operationImageName = if ($Scenario -in @('Success', 'Cancellation', 'AutoOnce')) {
    'ytec-product-job-cancellation-vm.exe'
}
else {
    'ytec-product-job-failure-vm.exe'
}
$guestRunnerImageName = if ($Scenario -in @('Success', 'Cancellation', 'AutoOnce')) {
    'run-cancel.cmd'
}
else {
    'run-failure.cmd'
}
$guestArgument = if ($Scenario -in @('Success', 'Cancellation', 'AutoOnce')) {
    "$profileArgument $scenarioArgument"
}
else {
    $scenarioArgument
}
$validationName = if ($Scenario -eq 'AutoOnce') {
    'product-job-auto-once'
}
elseif ($Scenario -eq 'Success') {
    'product-job-execution'
}
elseif ($Scenario -eq 'Cancellation') {
    'product-job-cancellation'
}
else {
    'product-restore-failure'
}
$evidenceCategory = if ($Scenario -eq 'AutoOnce') {
    'product-job-auto-once-vm'
}
elseif ($Scenario -eq 'Success') {
    'product-job-execution-vm'
}
elseif ($Scenario -eq 'Cancellation') {
    'product-job-cancellation-vm'
}
else {
    'product-restore-failure-vm'
}
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$evidenceRoot = Join-Path $repoRoot `
    ".validation\evidence\$evidenceCategory\$timestamp-$profileArgument-$scenarioArgument"
$targetVdi = Join-Path $evidenceRoot $targetFileName
$payloadViso = Join-Path $evidenceRoot 'product-job-validation-payload.viso'
$vmBeforePath = Join-Path $evidenceRoot 'vm-before.txt'
$resultPath = Join-Path $evidenceRoot 'result.json'

$productGuiExpectedSha256 = $null
$productGuiActualSha256 = $null
$requiredFiles = @(
    $vboxManage,
    $fixture,
    $operationHarness,
    $guestRunner
)
if ($Scenario -eq 'AutoOnce') {
    $requiredFiles += $productGui
}
foreach ($requiredFile in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "必要ファイルがありません: $requiredFile"
    }
}

if ($Scenario -eq 'AutoOnce') {
    $matrixRoot = Join-Path $repoRoot `
        '.validation\evidence\winpe-product-boot-matrix'
    $latestValidatedMatrix = Get-ChildItem -LiteralPath $matrixRoot `
        -Filter result.json -File -Recurse |
        Sort-Object LastWriteTimeUtc -Descending |
        ForEach-Object {
            $visualPath = Join-Path $_.DirectoryName 'visual-inspection.json'
            if (-not (Test-Path -LiteralPath $visualPath -PathType Leaf)) {
                return
            }
            $matrix = Get-Content -LiteralPath $_.FullName -Raw |
                ConvertFrom-Json
            $visual = Get-Content -LiteralPath $visualPath -Raw |
                ConvertFrom-Json
            if ($visual.result -eq 'PASS' -and $matrix.restored -eq $true -and
                $matrix.finalVmState -eq 'poweroff' -and
                $matrix.runningVmCountAfter -eq 0 -and
                $matrix.iso2011Ca.winpeGuiSha256 -eq
                    $matrix.iso2023Ca.winpeGuiSha256) {
                [pscustomobject]@{
                    ResultPath = $_.FullName
                    GuiSha256 = [string]$matrix.iso2011Ca.winpeGuiSha256
                }
            }
        } |
        Select-Object -First 1
    if ($null -eq $latestValidatedMatrix) {
        throw '視覚確認済みの最新製品WinPE起動マトリクスがありません。'
    }
    $productGuiExpectedSha256 = $latestValidatedMatrix.GuiSha256
    $productGuiActualSha256 =
        (Get-FileHash -Algorithm SHA256 -LiteralPath $productGui).Hash
    if ($productGuiActualSha256 -ne $productGuiExpectedSha256 -and
        -not $AllowPendingProductIsoRefresh) {
        throw '現行GUIは最新の視覚確認済み製品ISO内GUIと一致しません。ISOを再生成・再検証してください。'
    }
}

New-Item -ItemType Directory -Path $evidenceRoot -Force | Out-Null

function Invoke-VBox {
    param(
        [Parameter(Mandatory)][string[]] $Arguments,
        [Parameter(Mandatory)][string] $Operation
    )
    $output = @(& $vboxManage @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "VirtualBox操作が失敗しました: $Operation`n$($output -join "`n")"
    }
    return $output
}

function Get-VmInformation {
    param([Parameter(Mandatory)][string] $Name)
    return @(Invoke-VBox `
        -Arguments @('showvminfo', $Name, '--machinereadable') `
        -Operation "$Name 情報取得")
}

function Get-MachineValue {
    param(
        [Parameter(Mandatory)][string[]] $Information,
        [Parameter(Mandatory)][string] $Name
    )
    $line = $Information |
        Where-Object { $_ -like "$Name=*" } |
        Select-Object -First 1
    if ($line -match '^[^=]+="(.*)"$') {
        return $Matches[1]
    }
    return $null
}

function Get-AttachmentValue {
    param(
        [Parameter(Mandatory)][string[]] $Information,
        [Parameter(Mandatory)][string] $Controller,
        [Parameter(Mandatory)][int] $Port,
        [int] $Device = 0
    )
    $key = '"' + $Controller + '-' + $Port + '-' + $Device + '"'
    $line = $Information |
        Where-Object { $_ -like "$key=*" } |
        Select-Object -First 1
    if ($line -match '^"[^\"]+"="(.*)"$') {
        return $Matches[1].Replace('\\', '\')
    }
    return $null
}

function Assert-WorkspaceOrLabMedium {
    param([Parameter(Mandatory)][string] $Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path -eq 'none') {
        throw '既存媒体パスが空です。'
    }
    $canonical = [IO.Path]::GetFullPath($Path)
    $allowedRoots = [string[]]@(
        ([IO.Path]::GetFullPath($workspaceRoot) +
            [IO.Path]::DirectorySeparatorChar)
        ([IO.Path]::GetFullPath($labVmRoot) +
            [IO.Path]::DirectorySeparatorChar)
        ([IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'YTEC')) +
            [IO.Path]::DirectorySeparatorChar)
    )
    $matchingRoots = @($allowedRoots | Where-Object {
        $canonical.StartsWith($_, [StringComparison]::OrdinalIgnoreCase)
    })
    if ($matchingRoots.Count -eq 0 -or
        -not (Test-Path -LiteralPath $canonical -PathType Leaf)) {
        throw "許可された検証媒体ではありません: $canonical"
    }
    return $canonical
}

function Assert-NetworkDisabled {
    param([Parameter(Mandatory)][string[]] $Information)
    foreach ($index in 1..8) {
        $value = Get-MachineValue -Information $Information -Name "nic$index"
        if ($null -ne $value -and $value -ne 'none') {
            throw "VMのNIC$index が無効ではありません: $value"
        }
    }
}

function Get-VmState {
    param([Parameter(Mandatory)][string] $Name)
    return Get-MachineValue `
        -Information (Get-VmInformation -Name $Name) `
        -Name 'VMState'
}

function Save-Screenshot {
    param(
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][string] $Path,
        [switch] $BestEffort
    )
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Force
    }
    $output = @(& $vboxManage controlvm $Name screenshotpng $Path 2>&1)
    if ($LASTEXITCODE -ne 0 -and -not $BestEffort) {
        throw "VM画面を保存できませんでした: $Path`n$($output -join "`n")"
    }
    return $LASTEXITCODE -eq 0
}

function Send-GuestCommand {
    param(
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][string] $Command
    )
    $null = Invoke-VBox `
        -Arguments @('controlvm', $Name, 'keyboardputstring', $Command) `
        -Operation '固定キャンセルランナー入力'
    Start-Sleep -Seconds 1
    $null = Invoke-VBox `
        -Arguments @(
            'controlvm', $Name,
            'keyboardputscancode', '1c', '9c'
        ) `
        -Operation '固定キャンセルランナー開始'
}

function Set-BootOrder {
    param(
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][string[]] $Order
    )
    $arguments = [Collections.Generic.List[string]]::new()
    $null = $arguments.Add('modifyvm')
    $null = $arguments.Add($Name)
    for ($index = 0; $index -lt 4; ++$index) {
        $null = $arguments.Add('--boot' + ($index + 1))
        $null = $arguments.Add($Order[$index])
    }
    $null = Invoke-VBox -Arguments $arguments.ToArray() -Operation "$Name 起動順序設定"
}

function Attach-Medium {
    param(
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][string] $Controller,
        [Parameter(Mandatory)][int] $Port,
        [Parameter(Mandatory)][ValidateSet('hdd', 'dvddrive')][string] $Type,
        [Parameter(Mandatory)][string] $Medium,
        [switch] $NonRotational
    )
    $arguments = [Collections.Generic.List[string]]::new()
    foreach ($value in @(
        'storageattach', $Name,
        '--storagectl', $Controller,
        '--port', $Port.ToString(),
        '--device', '0',
        '--type', $Type,
        '--medium', $Medium
    )) {
        $null = $arguments.Add($value)
    }
    if ($Type -eq 'hdd') {
        $null = $arguments.Add('--nonrotational')
        $null = $arguments.Add($(if ($NonRotational) { 'on' } else { 'off' }))
    }
    $null = Invoke-VBox `
        -Arguments $arguments.ToArray() `
        -Operation "$Name $Controller port $Port 媒体接続"
}

function Detach-Medium {
    param(
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][string] $Controller,
        [Parameter(Mandatory)][int] $Port,
        [Parameter(Mandatory)][ValidateSet('hdd', 'dvddrive')][string] $Type
    )
    $null = Invoke-VBox `
        -Arguments @(
            'storageattach', $Name,
            '--storagectl', $Controller,
            '--port', $Port.ToString(),
            '--device', '0',
            '--type', $Type,
            '--medium', 'none'
        ) `
        -Operation "$Name $Controller port $Port 媒体切離し"
}

function Write-VisoDescriptor {
    param([Parameter(Mandatory)][string] $Path)
    $sources = @($fixture, $operationHarness, $guestRunner)
    if ($Scenario -eq 'AutoOnce') {
        $sources += $productGui
    }
    foreach ($source in $sources) {
        if ($source.Contains("'")) {
            throw "VISOへ安全に記述できないパスです: $source"
        }
    }
    $marker = [Guid]::NewGuid().ToString()
    $line = "--iprt-iso-maker-file-marker-bourne-sh $marker " +
        "--file-mode=0444 --dir-mode=0555 " +
        "'/ytec-product-job-fixture-vm.exe=$fixture' " +
        "'/$operationImageName=$operationHarness' " +
        "'/$guestRunnerImageName=$guestRunner'"
    if ($Scenario -eq 'AutoOnce') {
        $line += " '/ytec-winpe-gui.exe=$productGui'"
    }
    [IO.File]::WriteAllText(
        $Path,
        $line + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

$originalInformation = $null
$originalBootOrder = $null
$originalTarget = $null
$originalPayload = $null
$bootIso = $null
$vmStarted = $false
$configurationChanged = $false
$guestControlledShutdown = $false
$failureMessage = $null
$cleanupFailure = $null
$injectedAt = $null
$poweredOffAt = $null
$initialTargetPhysicalBytes = $null

try {
    $running = @(Invoke-VBox -Arguments @('list', 'runningvms') -Operation '稼働中VM確認')
    if ($running.Count -ne 0) {
        throw "別VMが稼働中のため開始しません: $($running -join ', ')"
    }

    $originalInformation = Get-VmInformation -Name $vmName
    $originalInformation | Set-Content -LiteralPath $vmBeforePath -Encoding UTF8
    if ((Get-MachineValue -Information $originalInformation -Name 'VMState') -ne 'poweroff') {
        throw '対象VMはpoweroffでなければなりません。'
    }
    if ((Get-MachineValue -Information $originalInformation -Name 'firmware') -ne
        $expectedFirmware) {
        throw "対象VMのfirmwareが一致しません: $expectedFirmware"
    }
    Assert-NetworkDisabled -Information $originalInformation
    if ($Profile -eq 'Clone' -and
        (Get-MachineValue -Information $originalInformation -Name 'SecureBoot') -ne 'on') {
        throw 'クローン用UEFI VMのSecure Bootが有効ではありません。'
    }

    $originalBootOrder = foreach ($index in 1..4) {
        Get-MachineValue -Information $originalInformation -Name "boot$index"
    }
    $invalidBootValues = @($originalBootOrder |
        Where-Object { [string]::IsNullOrWhiteSpace($_) })
    if ($originalBootOrder.Count -ne 4 -or $invalidBootValues.Count -ne 0) {
        throw '元の起動順序を取得できません。'
    }

    if ($Profile -eq 'Clone') {
        $source = Assert-WorkspaceOrLabMedium -Path `
            (Get-AttachmentValue -Information $originalInformation `
                -Controller 'SATA' -Port 0)
        $originalTarget = Assert-WorkspaceOrLabMedium -Path `
            (Get-AttachmentValue -Information $originalInformation `
                -Controller 'SATA' -Port 1)
        $bootIso = Assert-WorkspaceOrLabMedium -Path `
            (Get-AttachmentValue -Information $originalInformation `
                -Controller 'IDE' -Port 0)
        $originalPayload = Assert-WorkspaceOrLabMedium -Path `
            (Get-AttachmentValue -Information $originalInformation `
                -Controller 'IDE' -Port 1)
        if ($source -eq $originalTarget) {
            throw 'クローン用コピー元と元のコピー先が同一です。'
        }
    }
    else {
        $originalTarget = Assert-WorkspaceOrLabMedium -Path `
            (Get-AttachmentValue -Information $originalInformation `
                -Controller 'SATA' -Port 0)
        $carrierInformation = Get-VmInformation -Name $cloneVm
        if ((Get-MachineValue -Information $carrierInformation -Name 'VMState') -ne
            'poweroff') {
            throw 'WinPE ISO参照元VMはpoweroffでなければなりません。'
        }
        $bootIso = Assert-WorkspaceOrLabMedium -Path `
            (Get-AttachmentValue -Information $carrierInformation `
                -Controller 'IDE' -Port 0)
    }

    Write-VisoDescriptor -Path $payloadViso
    $null = Invoke-VBox `
        -Arguments @(
            'createmedium', 'disk',
            '--filename', $targetVdi,
            '--size', $targetSizeMb.ToString(),
            '--format', 'VDI',
            '--variant', 'Standard'
        ) `
        -Operation 'キャンセル試験専用合成VDI作成'
    $mediumInformation = @(Invoke-VBox `
        -Arguments @('showmediuminfo', 'disk', $targetVdi) `
        -Operation '合成VDI容量確認')
    if (-not ($mediumInformation -match "^Capacity:\s+$targetSizeMb MBytes$")) {
        throw "合成VDI容量が固定値と一致しません: $targetSizeMb MiB"
    }
    $initialTargetPhysicalBytes =
        (Get-Item -LiteralPath $targetVdi).Length

    $configurationChanged = $true
    if ($Profile -eq 'Clone') {
        Detach-Medium -Name $vmName -Controller 'IDE' -Port 1 -Type 'dvddrive'
        Detach-Medium -Name $vmName -Controller 'SATA' -Port 1 -Type 'hdd'
        Attach-Medium -Name $vmName -Controller 'IDE' -Port 1 `
            -Type 'dvddrive' -Medium $payloadViso
        Attach-Medium -Name $vmName -Controller 'SATA' -Port 1 `
            -Type 'hdd' -Medium $targetVdi -NonRotational
    }
    else {
        Detach-Medium -Name $vmName -Controller 'SATA' -Port 0 -Type 'hdd'
        Attach-Medium -Name $vmName -Controller 'SATA' -Port 0 `
            -Type 'hdd' -Medium $targetVdi -NonRotational
        Attach-Medium -Name $vmName -Controller 'SATA' -Port 1 `
            -Type 'dvddrive' -Medium $bootIso
        Attach-Medium -Name $vmName -Controller 'SATA' -Port 2 `
            -Type 'dvddrive' -Medium $payloadViso
        Set-BootOrder -Name $vmName -Order @('dvd', 'none', 'none', 'none')
    }
    $preparedInformation = Get-VmInformation -Name $vmName
    Assert-NetworkDisabled -Information $preparedInformation
    $null = Invoke-VBox `
        -Arguments @('startvm', $vmName, '--type', 'headless') `
        -Operation "$vmName headless起動"
    $vmStarted = $true

    foreach ($attempt in 1..15) {
        Start-Sleep -Milliseconds 750
        $null = Invoke-VBox `
            -Arguments @(
                'controlvm', $vmName,
                'keyboardputscancode', '39', 'b9'
            ) `
            -Operation 'WinPE DVD起動キー送信'
    }
    Start-Sleep -Seconds $BootWaitSeconds
    if ((Get-VmState -Name $vmName) -ne 'running') {
        throw 'コマンド投入前にVMがrunningではなくなりました。'
    }
    $null = Save-Screenshot -Name $vmName `
        -Path (Join-Path $evidenceRoot 'vm-booted.png')

    Start-Sleep -Seconds 5
    $guestCommand = 'for %D in (D E F G H I J K L M N O P Q R S T U V W Y Z) ' +
        "do @if exist %D:\$guestRunnerImageName call " +
        "%D:\$guestRunnerImageName $guestArgument"
    Send-GuestCommand -Name $vmName -Command $guestCommand
    $injectedAt = Get-Date
    Start-Sleep -Seconds 3
    $null = Save-Screenshot -Name $vmName `
        -Path (Join-Path $evidenceRoot 'vm-command-delivered.png') `
        -BestEffort

    Start-Sleep -Seconds 9
    if ($Scenario -in @('Cancellation', 'AutoOnce') -and
        (Get-VmState -Name $vmName) -eq 'running' -and
        (Get-Item -LiteralPath $targetVdi).Length -le
            $initialTargetPhysicalBytes) {
        $null = Save-Screenshot -Name $vmName `
            -Path (Join-Path $evidenceRoot 'vm-command-retry-needed.png') `
            -BestEffort
        Send-GuestCommand -Name $vmName -Command $guestCommand
        $injectedAt = Get-Date
        Start-Sleep -Seconds 3
        $null = Save-Screenshot -Name $vmName `
            -Path (Join-Path $evidenceRoot 'vm-command-retried.png') `
            -BestEffort
    }
    if ($Scenario -eq 'AutoOnce') {
        Start-Sleep -Seconds 6
        if ((Get-VmState -Name $vmName) -eq 'running' -and
            (Get-Item -LiteralPath $targetVdi).Length -le
                $initialTargetPhysicalBytes) {
            Send-GuestCommand -Name $vmName -Command $guestCommand
            $injectedAt = Get-Date
        }
    }

    $deadline = $injectedAt.AddMinutes($TimeoutMinutes)
    $screenIndex = 0
    while ((Get-Date) -lt $deadline) {
        $state = Get-VmState -Name $vmName
        if ($state -eq 'poweroff') {
            $poweredOffAt = Get-Date
            break
        }
        if ($state -ne 'running') {
            throw "キャンセル試験中に予期しないVM状態になりました: $state"
        }
        $screenIndex = ($screenIndex + 1) % 5
        $null = Save-Screenshot -Name $vmName `
            -Path (Join-Path $evidenceRoot `
                ('vm-final-window-{0}.png' -f $screenIndex)) `
            -BestEffort
        Start-Sleep -Seconds 3
    }
    if ($null -eq $poweredOffAt) {
        throw "ゲストの制御済みPASS終了を${TimeoutMinutes}分以内に確認できませんでした。"
    }
    $elapsedAfterInjection = ($poweredOffAt - $injectedAt).TotalSeconds
    if ($elapsedAfterInjection -lt 15) {
        throw "PASS表示待機より早くVMが終了しました: $elapsedAfterInjection 秒"
    }
    $guestControlledShutdown = $true
}
catch {
    $failureMessage = $_.Exception.Message
}
finally {
    try {
        if ($vmStarted -and (Get-VmState -Name $vmName) -eq 'running') {
            $null = Save-Screenshot -Name $vmName `
                -Path (Join-Path $evidenceRoot 'vm-failure.png') `
                -BestEffort
            $null = Invoke-VBox `
                -Arguments @('controlvm', $vmName, 'poweroff') `
                -Operation "$vmName 失敗時停止"
        }
        if ($configurationChanged) {
            if ($Profile -eq 'Clone') {
                Detach-Medium -Name $vmName -Controller 'IDE' -Port 1 `
                    -Type 'dvddrive'
                Detach-Medium -Name $vmName -Controller 'SATA' -Port 1 `
                    -Type 'hdd'
                Attach-Medium -Name $vmName -Controller 'IDE' -Port 1 `
                    -Type 'dvddrive' -Medium $originalPayload
                Attach-Medium -Name $vmName -Controller 'SATA' -Port 1 `
                    -Type 'hdd' -Medium $originalTarget -NonRotational
            }
            else {
                Detach-Medium -Name $vmName -Controller 'SATA' -Port 0 `
                    -Type 'hdd'
                Detach-Medium -Name $vmName -Controller 'SATA' -Port 1 `
                    -Type 'dvddrive'
                Detach-Medium -Name $vmName -Controller 'SATA' -Port 2 `
                    -Type 'dvddrive'
                Attach-Medium -Name $vmName -Controller 'SATA' -Port 0 `
                    -Type 'hdd' -Medium $originalTarget
                Set-BootOrder -Name $vmName -Order $originalBootOrder
            }
        }
    }
    catch {
        $cleanupFailure = $_.Exception.Message
    }
}

$restored = $false
$finalState = $null
$runningAfter = @()
try {
    $finalInformation = Get-VmInformation -Name $vmName
    $finalState = Get-MachineValue -Information $finalInformation -Name 'VMState'
    Assert-NetworkDisabled -Information $finalInformation
    if ($Profile -eq 'Clone') {
        $restored =
            (Get-AttachmentValue -Information $finalInformation `
                -Controller 'IDE' -Port 1) -eq $originalPayload -and
            (Get-AttachmentValue -Information $finalInformation `
                -Controller 'SATA' -Port 1) -eq $originalTarget
    }
    else {
        $restored =
            (Get-AttachmentValue -Information $finalInformation `
                -Controller 'SATA' -Port 0) -eq $originalTarget -and
            (Get-AttachmentValue -Information $finalInformation `
                -Controller 'SATA' -Port 1) -eq 'none' -and
            (Get-AttachmentValue -Information $finalInformation `
                -Controller 'SATA' -Port 2) -eq 'none'
    }
    $runningAfter = @(Invoke-VBox `
        -Arguments @('list', 'runningvms') `
        -Operation '完了後稼働VM確認')
}
catch {
    if ([string]::IsNullOrWhiteSpace($cleanupFailure)) {
        $cleanupFailure = $_.Exception.Message
    }
}

$screenshots = @(Get-ChildItem -LiteralPath $evidenceRoot -Filter '*.png' `
    -File -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object {
        [ordered]@{
            name = $_.Name
            bytes = $_.Length
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash
        }
    })
$targetEvidence = if (Test-Path -LiteralPath $targetVdi -PathType Leaf) {
    [ordered]@{
        path = $targetVdi
        bytes = (Get-Item -LiteralPath $targetVdi).Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $targetVdi).Hash
        retained = $true
    }
}
else {
    $null
}
$elapsedSeconds = if ($null -ne $injectedAt -and $null -ne $poweredOffAt) {
    [math]::Round(($poweredOffAt - $injectedAt).TotalSeconds, 3)
}
else {
    $null
}
$passed = $guestControlledShutdown -and
    [string]::IsNullOrWhiteSpace($failureMessage) -and
    [string]::IsNullOrWhiteSpace($cleanupFailure) -and
    $restored -and $finalState -eq 'poweroff' -and $runningAfter.Count -eq 0
$result = [ordered]@{
    schemaVersion = 1
    result = if ($passed) { 'PASS' } else { 'FAIL' }
    product = 'Y-TEC Tsumugi Drive'
    validation = $validationName
    profile = $profileArgument
    scenario = $scenarioArgument
    expectedMarker = $expectedMarker
    vm = $vmName
    firmware = $expectedFirmware
    secureBoot = if ($Profile -eq 'Clone') { $true } else { $false }
    networkAdapters = 'none'
    physicalDiskOrUsbUsed = $false
    guestControlledShutdown = $guestControlledShutdown
    elapsedAfterInjectionSeconds = $elapsedSeconds
    target = $targetEvidence
    executables = [ordered]@{
        fixtureSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $fixture).Hash
        operationSha256 =
            (Get-FileHash -Algorithm SHA256 -LiteralPath $operationHarness).Hash
        productGuiSha256 = $productGuiActualSha256
        expectedProductIsoGuiSha256 = $productGuiExpectedSha256
        productIsoGuiHashMatched =
            $productGuiActualSha256 -eq $productGuiExpectedSha256
        pendingProductIsoRefreshAllowed =
            [bool]$AllowPendingProductIsoRefresh
        destructiveVmBuildOnly = $true
    }
    screenshots = $screenshots
    cleanup = [ordered]@{
        finalVmState = $finalState
        originalAttachmentsRestored = $restored
        targetVdiDetachedAndRetained = $null -ne $targetEvidence
        runningVmCount = $runningAfter.Count
    }
    failure = $failureMessage
    cleanupFailure = $cleanupFailure
    completedUtc = [DateTimeOffset]::UtcNow.ToString('o')
}
$result | ConvertTo-Json -Depth 8 | Set-Content `
    -LiteralPath $resultPath -Encoding UTF8

if (-not $passed) {
    throw "製品ジョブVM試験が失敗しました。証跡: $evidenceRoot"
}

Write-Host "Product job VM test ($Profile / $Scenario): PASS"
Write-Host "Evidence: $evidenceRoot"
