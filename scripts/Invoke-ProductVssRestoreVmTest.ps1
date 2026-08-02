[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $BootIsoPath,

    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string] $ExpectedBootIsoSha256,

    [ValidateRange(15, 180)]
    [int] $BootWaitSeconds = 30,

    [ValidateRange(30, 180)]
    [int] $RestoreTimeoutMinutes = 90,

    [ValidateRange(3, 20)]
    [int] $WindowsBootTimeoutMinutes = 10
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot '..\..'))
$labRoot = Join-Path $workspaceRoot 'business-apps\ytec-windows-backup'
$credentialRoot = Join-Path $labRoot '.validation\vm-secrets'
$vboxManage = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$vmName = 'YDC-WinPE-Product-Preflight'
$guestUser = 'YbcTest'
$passwordFile = Join-Path $credentialRoot `
    'YWB-Win10-22H2-x64-Clean.password.txt'
$guestPowerShell = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
$harness = Join-Path $repoRoot (
    'out\build\msvc-x64-vm-destructive\tests\' +
    'ytec-product-vss-restore-vm.exe'
)
$guestRunner = Join-Path $repoRoot (
    'scripts\vm\Run-ProductVssRestoreValidation.cmd'
)
$protectedImageVdi = Join-Path $repoRoot (
    '.validation\vms\YDC-Phase5-VSS-x64\' +
    'YDC-Product-VSS-Output-32GiB-20260731-235312.vdi'
)
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$evidenceRoot = Join-Path $repoRoot (
    ".validation\evidence\product-vss-restore-vm\$timestamp"
)
$workingImageVdi = Join-Path $evidenceRoot 'working-image-media-32GiB.vdi'
$targetVdi = Join-Path $evidenceRoot 'restored-target-96GiB.vdi'
$payloadViso = Join-Path $evidenceRoot 'vss-restore-payload.viso'
$serialEvidencePath = Join-Path $evidenceRoot 'uart.txt'
$resultPath = Join-Path $evidenceRoot 'result.json'
$nvramBackup = Join-Path $evidenceRoot 'worker-nvram-before.bin'
$imageCapacityMb = 32768
$targetCapacityMb = 98304
$expectedRestoreMarker = 'YDC_PRODUCT_VSS_RESTORE_PASS'
$expectedTargetMarker = 'YDC_TARGET_SECURE_BOOT_PASS_V1'

function Invoke-VBox {
    param(
        [Parameter(Mandatory)][string[]] $Arguments,
        [Parameter(Mandatory)][string] $Operation,
        [switch] $BestEffort
    )
    $output = @(& $vboxManage @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -and -not $BestEffort) {
        throw "$Operation failed (VBoxManage exit $exitCode):`n$($output -join "`n")"
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $output
    }
}

function Get-VmInformation {
    return (Invoke-VBox `
        -Arguments @('showvminfo', $vmName, '--machinereadable') `
        -Operation 'Read worker VM information').Output
}

function Get-MachineValue {
    param(
        [Parameter(Mandatory)][string[]] $Information,
        [Parameter(Mandatory)][string] $Name
    )
    $line = $Information | Where-Object { $_ -like "$Name=*" } |
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
        [Parameter(Mandatory)][int] $Port
    )
    $key = '"' + $Controller + '-' + $Port + '-0"'
    $line = $Information | Where-Object { $_ -like "$key=*" } |
        Select-Object -First 1
    if ($line -match '^"[^"]+"="(.*)"$') {
        return $Matches[1].Replace('\\', '\')
    }
    return $null
}

function Get-VmState {
    return Get-MachineValue -Information (Get-VmInformation) -Name 'VMState'
}

function Assert-NetworkDisabled {
    param([Parameter(Mandatory)][string[]] $Information)
    foreach ($index in 1..8) {
        $value = Get-MachineValue -Information $Information -Name "nic$index"
        if ($null -ne $value -and $value -ne 'none') {
            throw "NIC$index is not disabled: $value"
        }
    }
}

function Assert-AllowedFile {
    param([Parameter(Mandatory)][string] $Path)
    $canonical = [IO.Path]::GetFullPath($Path)
    $allowedRoots = @(
        ([IO.Path]::GetFullPath($workspaceRoot) +
            [IO.Path]::DirectorySeparatorChar)
        ([IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'YTEC')) +
            [IO.Path]::DirectorySeparatorChar)
    )
    if (-not (Test-Path -LiteralPath $canonical -PathType Leaf) -or
        @($allowedRoots | Where-Object {
            $canonical.StartsWith($_, [StringComparison]::OrdinalIgnoreCase)
        }).Count -eq 0) {
        throw "File is outside the allowed validation roots: $canonical"
    }
    return $canonical
}

function Assert-MediumCapacity {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][int] $CapacityMb
    )
    $information = (Invoke-VBox `
        -Arguments @('showmediuminfo', 'disk', $Path) `
        -Operation "Read medium capacity: $Path").Output
    if (-not ($information -match "^Capacity:\s+$CapacityMb MBytes$")) {
        throw "Unexpected virtual disk capacity: $Path"
    }
}

function Attach-Medium {
    param(
        [Parameter(Mandatory)][string] $Controller,
        [Parameter(Mandatory)][int] $Port,
        [Parameter(Mandatory)][ValidateSet('hdd', 'dvddrive')][string] $Type,
        [Parameter(Mandatory)][string] $Medium,
        [switch] $NonRotational
    )
    $arguments = [Collections.Generic.List[string]]::new()
    foreach ($value in @(
        'storageattach', $vmName,
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
        $null = $arguments.Add('--discard')
        $null = $arguments.Add('off')
    }
    $null = Invoke-VBox `
        -Arguments $arguments.ToArray() `
        -Operation "$Controller port $Port medium attach"
}

function Detach-Medium {
    param(
        [Parameter(Mandatory)][string] $Controller,
        [Parameter(Mandatory)][int] $Port,
        [Parameter(Mandatory)][ValidateSet('hdd', 'dvddrive')][string] $Type,
        [switch] $BestEffort
    )
    $null = Invoke-VBox `
        -Arguments @(
            'storageattach', $vmName,
            '--storagectl', $Controller,
            '--port', $Port.ToString(),
            '--device', '0',
            '--type', $Type,
            '--medium', 'none'
        ) `
        -Operation "$Controller port $Port medium detach" `
        -BestEffort:$BestEffort
}

function Set-BootOrder {
    param([Parameter(Mandatory)][string[]] $Order)
    $arguments = [Collections.Generic.List[string]]::new()
    $arguments.Add('modifyvm')
    $arguments.Add($vmName)
    foreach ($index in 0..3) {
        $arguments.Add('--boot' + ($index + 1))
        $arguments.Add($Order[$index])
    }
    $null = Invoke-VBox `
        -Arguments $arguments.ToArray() `
        -Operation 'Set worker boot order'
}

function Set-SecureBoot {
    param([Parameter(Mandatory)][bool] $Enabled)
    $setting = if ($Enabled) { '--enable' } else { '--disable' }
    $null = Invoke-VBox `
        -Arguments @('modifynvram', $vmName, 'secureboot', $setting) `
        -Operation 'Set worker Secure Boot state'
}

function Enable-SerialEvidence {
    $null = Invoke-VBox `
        -Arguments @(
            'modifyvm', $vmName,
            '--uart1', '0x3F8', '4',
            '--uartmode1', 'file', $serialEvidencePath
        ) `
        -Operation 'Enable UART evidence'
}

function Disable-SerialEvidence {
    param([switch] $BestEffort)
    $null = Invoke-VBox `
        -Arguments @('modifyvm', $vmName, '--uart1', 'off') `
        -Operation 'Disable UART evidence' `
        -BestEffort:$BestEffort
}

function Save-Screenshot {
    param([Parameter(Mandatory)][string] $Name)
    $path = Join-Path $evidenceRoot $Name
    $result = Invoke-VBox `
        -Arguments @('controlvm', $vmName, 'screenshotpng', $path) `
        -Operation "Save screenshot $Name" `
        -BestEffort
    return $result.ExitCode -eq 0
}

function Test-TargetGuestControlReady {
    $probe = Invoke-VBox `
        -Arguments @(
            'guestcontrol', $vmName, 'stat',
            "--username=$guestUser",
            "--passwordfile=$passwordFile",
            $guestPowerShell
        ) `
        -Operation 'Probe restored target GuestControl' `
        -BestEffort
    if ($probe.ExitCode -eq 0) {
        return $true
    }
    $null = Invoke-VBox `
        -Arguments @('guestcontrol', $vmName, 'closesession', '--all') `
        -Operation 'Close stale restored target GuestControl sessions' `
        -BestEffort
    return $false
}

function Invoke-TargetValidation {
    param([Parameter(Mandatory)][string] $EncodedCommand)
    $result = Invoke-VBox `
        -Arguments @(
            'guestcontrol', $vmName, 'run',
            "--exe=$guestPowerShell",
            '--arg0=powershell.exe',
            "--username=$guestUser",
            "--passwordfile=$passwordFile",
            '--wait-stdout',
            '--wait-stderr',
            '--',
            '-NoLogo',
            '-NoProfile',
            '-NonInteractive',
            '-ExecutionPolicy',
            'Bypass',
            '-EncodedCommand',
            $EncodedCommand
        ) `
        -Operation 'Run fixed restored target validation probe'
    if ($result.Output -notcontains 'YDC_TARGET_PROBE_LAUNCHED') {
        throw 'Restored target GuestControl launch marker is missing.'
    }
}

function Read-UartRecords {
    if (-not (Test-Path -LiteralPath $serialEvidencePath -PathType Leaf)) {
        return @()
    }
    try {
        $stream = [IO.File]::Open(
            $serialEvidencePath,
            [IO.FileMode]::Open,
            [IO.FileAccess]::Read,
            [IO.FileShare]::ReadWrite
        )
        try {
            $bytes = New-Object byte[] $stream.Length
            $read = $stream.Read($bytes, 0, $bytes.Length)
            if ($read -ne $bytes.Length) {
                return @()
            }
        }
        finally {
            $stream.Dispose()
        }
    }
    catch [IO.IOException] {
        return @()
    }
    $serialText = [Text.Encoding]::ASCII.GetString($bytes)
    $ansiPattern = [regex]::Escape([string][char]27) +
        '\[[0-9;=?]*[A-Za-z]'
    $clean = [regex]::Replace($serialText, $ansiPattern, '')
    return @([regex]::Matches($clean, 'YDCUART=([^\r\n]*?);END') |
        ForEach-Object { $_.Groups[1].Value })
}

function Write-VisoDescriptor {
    if ($harness.Contains("'") -or $guestRunner.Contains("'")) {
        throw 'VISO payload path cannot contain a single quote.'
    }
    $marker = [Guid]::NewGuid().ToString()
    $line = "--iprt-iso-maker-file-marker-bourne-sh $marker " +
        "--file-mode=0444 --dir-mode=0555 " +
        "'/ytec-product-vss-restore-vm.exe=$harness' " +
        "'/Run-ProductVssRestoreValidation.cmd=$guestRunner'"
    [IO.File]::WriteAllText(
        $payloadViso,
        $line + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false)
    )
}

foreach ($required in @(
    $vboxManage, $passwordFile, $harness, $guestRunner, $protectedImageVdi
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required validation file is missing: $required"
    }
}
$passwordFile = Assert-AllowedFile -Path $passwordFile
if ((Get-Item -LiteralPath $passwordFile).Attributes -band
    [IO.FileAttributes]::ReparsePoint) {
    throw 'The fixed GuestControl password file must not be a reparse point.'
}
$bootIso = Assert-AllowedFile -Path $BootIsoPath
$bootIsoHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $bootIso).Hash
if ($bootIsoHash -ine $ExpectedBootIsoSha256) {
    throw 'The WinPE boot ISO hash does not match the approved input.'
}
$runningVms = @((Invoke-VBox `
    -Arguments @('list', 'runningvms') `
    -Operation 'Check running VMs').Output)
if ($runningVms.Count -ne 0) {
    throw 'Another VM is already running.'
}

$originalInformation = Get-VmInformation
$originalState = Get-MachineValue $originalInformation 'VMState'
if ($originalState -ne 'poweroff' -or
    (Get-MachineValue $originalInformation 'firmware') -ne 'EFI64' -or
    (Get-MachineValue $originalInformation 'SecureBoot') -ne 'on' -or
    (Get-MachineValue $originalInformation 'uart1') -ne 'off' -or
    (Get-MachineValue $originalInformation 'uart2') -ne 'off') {
    throw 'The fixed worker must be powered off in EFI64/Secure Boot mode with UART off.'
}
Assert-NetworkDisabled -Information $originalInformation
$originalBootOrder = foreach ($index in 1..4) {
    Get-MachineValue $originalInformation "boot$index"
}
$originalIde0 = Assert-AllowedFile -Path (
    Get-AttachmentValue $originalInformation 'IDE' 0
)
$originalIde1 = Assert-AllowedFile -Path (
    Get-AttachmentValue $originalInformation 'IDE' 1
)
$originalSata0 = Assert-AllowedFile -Path (
    Get-AttachmentValue $originalInformation 'SATA' 0
)
$originalSata1 = Assert-AllowedFile -Path (
    Get-AttachmentValue $originalInformation 'SATA' 1
)
$nvramFile = Get-MachineValue $originalInformation 'NvramFile'
if (-not (Test-Path -LiteralPath $nvramFile -PathType Leaf)) {
    throw 'The worker NVRAM file is missing.'
}

New-Item -ItemType Directory -Path $evidenceRoot -Force | Out-Null
$originalInformation | Set-Content `
    -LiteralPath (Join-Path $evidenceRoot 'vm-before.txt') -Encoding UTF8
Copy-Item -LiteralPath $nvramFile -Destination $nvramBackup
$configurationChanged = $false
$vmStarted = $false
$cleanupRestored = $false
$failure = $null
$cleanupFailure = $null
$protectedHashBefore = $null
$protectedHashAfter = $null
$restoreMarkerVerified = $false
$targetBootVerified = $false
$targetGuestFields = @()
$targetGuestShutdownObserved = $false
$targetForcedStopAfterValidation = $false
$targetProbeStarted = $false

try {
    $protectedHashBefore = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $protectedImageVdi
    ).Hash
    Write-VisoDescriptor
    $null = Invoke-VBox `
        -Arguments @(
            'clonemedium', 'disk', $protectedImageVdi, $workingImageVdi,
            '--format', 'VDI', '--variant', 'Standard'
        ) `
        -Operation 'Clone protected VSS image media VDI'
    $null = Invoke-VBox `
        -Arguments @(
            'createmedium', 'disk', '--filename', $targetVdi,
            '--size', $targetCapacityMb.ToString(),
            '--format', 'VDI', '--variant', 'Standard'
        ) `
        -Operation 'Create disposable 96GiB RAW restore target'
    Assert-MediumCapacity -Path $workingImageVdi -CapacityMb $imageCapacityMb
    Assert-MediumCapacity -Path $targetVdi -CapacityMb $targetCapacityMb

    $configurationChanged = $true
    Detach-Medium -Controller 'IDE' -Port 0 -Type 'dvddrive'
    Detach-Medium -Controller 'IDE' -Port 1 -Type 'dvddrive'
    Detach-Medium -Controller 'SATA' -Port 0 -Type 'hdd'
    Detach-Medium -Controller 'SATA' -Port 1 -Type 'hdd'
    Attach-Medium -Controller 'IDE' -Port 0 -Type 'dvddrive' -Medium $bootIso
    Attach-Medium -Controller 'IDE' -Port 1 -Type 'dvddrive' -Medium $payloadViso
    Attach-Medium -Controller 'SATA' -Port 0 -Type 'hdd' `
        -Medium $workingImageVdi -NonRotational
    Attach-Medium -Controller 'SATA' -Port 1 -Type 'hdd' `
        -Medium $targetVdi -NonRotational
    Set-BootOrder -Order @('dvd', 'none', 'none', 'none')
    Enable-SerialEvidence
    Assert-NetworkDisabled -Information (Get-VmInformation)

    $null = Invoke-VBox `
        -Arguments @('startvm', $vmName, '--type', 'headless') `
        -Operation 'Start product VSS restore WinPE'
    $vmStarted = $true
    foreach ($attempt in 1..15) {
        Start-Sleep -Milliseconds 750
        $null = Invoke-VBox `
            -Arguments @(
                'controlvm', $vmName, 'keyboardputscancode', '39', 'b9'
            ) `
            -Operation 'Send WinPE DVD boot key'
    }
    Start-Sleep -Seconds $BootWaitSeconds
    $null = Save-Screenshot -Name 'winpe-ready.png'
    $validationStartedAt = Get-Date
    $deadline = $validationStartedAt.AddMinutes($RestoreTimeoutMinutes)
    $screenIndex = 0
    while ((Get-Date) -lt $deadline) {
        $state = Get-VmState
        if ($state -eq 'poweroff') {
            break
        }
        if ($state -ne 'running') {
            throw "Unexpected VM state during restore: $state"
        }
        if ($screenIndex % 6 -eq 0) {
            Write-Host (
                'VSS restore VM is running: {0:N1} minutes elapsed' -f
                ((Get-Date) - $validationStartedAt).TotalMinutes
            )
        }
        $null = Save-Screenshot -Name (
            'restore-progress-{0:D3}.png' -f $screenIndex
        )
        ++$screenIndex
        Start-Sleep -Seconds 10
    }
    if ((Get-VmState) -ne 'poweroff') {
        throw "VSS restore did not finish within $RestoreTimeoutMinutes minutes."
    }
    $vmStarted = $false
    $records = Read-UartRecords
    $restoreMarkerVerified = @($records | Where-Object {
        $_ -eq $expectedRestoreMarker
    }).Count -ne 0
    if (-not $restoreMarkerVerified) {
        Write-Warning (
            'WinPEのCOMデバイスを利用できないため、復元完了は新規RAW対象の' +
            'ターゲット単体Secure Bootで検証します。'
        )
    }

    Detach-Medium -Controller 'IDE' -Port 0 -Type 'dvddrive'
    Detach-Medium -Controller 'IDE' -Port 1 -Type 'dvddrive'
    Detach-Medium -Controller 'SATA' -Port 0 -Type 'hdd'
    Detach-Medium -Controller 'SATA' -Port 1 -Type 'hdd'
    Attach-Medium -Controller 'SATA' -Port 0 -Type 'hdd' `
        -Medium $targetVdi -NonRotational
    Set-BootOrder -Order @('disk', 'none', 'none', 'none')
    Set-SecureBoot -Enabled $true

    $guestCheck = @'
$ErrorActionPreference='Stop'
$diskStyle='ERROR'
$isBoot=0
$isSystem=0
$partitionCount=0
$espCount=0
$msrCount=0
$secureBoot=0
$architecture64=0
$probeError=0
$shutdownScheduled=0
try {
    $disk=Get-Disk -Number 0
    $parts=@(Get-Partition -DiskNumber 0)
    $esp=@($parts | Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' })
    $msr=@($parts | Where-Object { $_.GptType -eq '{e3c9e316-0b5c-4db8-817d-f92df00215ae}' })
    $diskStyle=[string]$disk.PartitionStyle
    $isBoot=[int][bool]$disk.IsBoot
    $isSystem=[int][bool]$disk.IsSystem
    $partitionCount=$parts.Count
    $espCount=$esp.Count
    $msrCount=$msr.Count
    $secureBoot=[int](Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -Name UEFISecureBootEnabled).UEFISecureBootEnabled
    $architecture64=[int]([string](Get-CimInstance Win32_OperatingSystem).OSArchitecture -match '64')
}
catch {
    $probeError=1
}
& "$env:SystemRoot\System32\shutdown.exe" /s /t 5 | Out-Null
if ($LASTEXITCODE -eq 0) {
    $shutdownScheduled=1
}
$passed=(
    $probeError -eq 0 -and $diskStyle -eq 'GPT' -and
    $isBoot -eq 1 -and $isSystem -eq 1 -and
    $espCount -eq 1 -and $msrCount -eq 1 -and
    $secureBoot -eq 1 -and $architecture64 -eq 1 -and
    $shutdownScheduled -eq 1
)
$marker=if($passed){'YDC_TARGET_SECURE_BOOT_PASS_V1'}else{'YDC_TARGET_SECURE_BOOT_FAIL_V1'}
$fields=@(
    $marker,('DiskStyle='+$diskStyle),('IsBoot='+$isBoot),
    ('IsSystem='+$isSystem),('PartitionCount='+$partitionCount),
    ('EspCount='+$espCount),('MsrCount='+$msrCount),
    ('SecureBoot='+$secureBoot),('Architecture64='+$architecture64),
    ('ProbeError='+$probeError),('ShutdownScheduled='+$shutdownScheduled)
)
$payload='YDCUART='+($fields -join ';')+';END'
& $env:ComSpec /d /c ('echo '+$payload+' >COM1') | Out-Null
Write-Output 'YDC_TARGET_PROBE_LAUNCHED'
'@.Trim()
    $encodedGuestCheck = [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes($guestCheck)
    )
    $null = Invoke-VBox `
        -Arguments @('startvm', $vmName, '--type', 'headless') `
        -Operation 'Start restored target only with Secure Boot'
    $vmStarted = $true
    $targetDeadline = (Get-Date).AddMinutes($WindowsBootTimeoutMinutes)
    $targetScreen = 0
    $targetRecord = $null
    while ((Get-Date) -lt $targetDeadline) {
        $state = Get-VmState
        if ($state -eq 'poweroff') {
            break
        }
        if ($state -ne 'running') {
            throw "Unexpected restored target VM state: $state"
        }
        $null = Save-Screenshot -Name (
            'target-secure-boot-{0:D3}.png' -f $targetScreen
        )
        ++$targetScreen
        if (-not $targetProbeStarted -and
            (Test-TargetGuestControlReady)) {
            Invoke-TargetValidation -EncodedCommand $encodedGuestCheck
            $targetProbeStarted = $true
        }
        $targetRecord = @(Read-UartRecords | Where-Object {
            $_.StartsWith($expectedTargetMarker, [StringComparison]::Ordinal)
        }) | Select-Object -Last 1
        if (-not [string]::IsNullOrWhiteSpace($targetRecord)) {
            break
        }
        Start-Sleep -Seconds 10
    }
    if (-not [string]::IsNullOrWhiteSpace($targetRecord)) {
        $shutdownDeadline = (Get-Date).AddSeconds(30)
        while ((Get-Date) -lt $shutdownDeadline -and
            (Get-VmState) -eq 'running') {
            Start-Sleep -Seconds 1
        }
    }
    $targetGuestShutdownObserved = (Get-VmState) -eq 'poweroff'
    if ($targetGuestShutdownObserved) {
        $vmStarted = $false
    }
    elseif ([string]::IsNullOrWhiteSpace($targetRecord)) {
        $null = Save-Screenshot -Name 'target-validation-window-ended.png'
        $null = Invoke-VBox `
            -Arguments @('controlvm', $vmName, 'poweroff') `
            -Operation 'Stop disposable target to read completed UART evidence'
        $vmStarted = $false
        $targetForcedStopAfterValidation = $true
    }
    if ([string]::IsNullOrWhiteSpace($targetRecord)) {
        $targetRecord = @(Read-UartRecords | Where-Object {
            $_.StartsWith($expectedTargetMarker, [StringComparison]::Ordinal)
        }) | Select-Object -Last 1
    }
    if ([string]::IsNullOrWhiteSpace($targetRecord)) {
        throw 'Restored target UART validation PASS record is missing.'
    }
    $targetGuestFields = @($targetRecord -split ';')
    foreach ($requiredField in @(
        $expectedTargetMarker,
        'DiskStyle=GPT',
        'IsBoot=1',
        'IsSystem=1',
        'EspCount=1',
        'MsrCount=1',
        'SecureBoot=1',
        'Architecture64=1',
        'ProbeError=0',
        'ShutdownScheduled=1'
    )) {
        if ($targetGuestFields -notcontains $requiredField) {
            throw "Restored target field missing: $requiredField"
        }
    }
    $targetBootVerified = $true
    if ((Get-VmState) -eq 'poweroff') {
        $vmStarted = $false
    }
    else {
        $null = Invoke-VBox `
            -Arguments @('controlvm', $vmName, 'poweroff') `
            -Operation 'Stop disposable target after completed validation'
        $vmStarted = $false
        $targetForcedStopAfterValidation = $true
    }
    $protectedHashAfter = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $protectedImageVdi
    ).Hash
    if ($protectedHashBefore -ine $protectedHashAfter) {
        throw 'The protected VSS image media VDI changed.'
    }
}
catch {
    $failure = $_.Exception.Message
}
finally {
    try {
        if ($vmStarted -and (Get-VmState) -eq 'running') {
            $null = Save-Screenshot -Name 'failure.png'
            $null = Invoke-VBox `
                -Arguments @('controlvm', $vmName, 'poweroff') `
                -Operation 'Stop failed disposable VM execution'
        }
        if ($configurationChanged) {
            foreach ($entry in @(
                @('IDE', 0, 'dvddrive'),
                @('IDE', 1, 'dvddrive'),
                @('SATA', 0, 'hdd'),
                @('SATA', 1, 'hdd')
            )) {
                Detach-Medium -Controller $entry[0] -Port $entry[1] `
                    -Type $entry[2] -BestEffort
            }
            Attach-Medium -Controller 'IDE' -Port 0 -Type 'dvddrive' `
                -Medium $originalIde0
            Attach-Medium -Controller 'IDE' -Port 1 -Type 'dvddrive' `
                -Medium $originalIde1
            Attach-Medium -Controller 'SATA' -Port 0 -Type 'hdd' `
                -Medium $originalSata0
            Attach-Medium -Controller 'SATA' -Port 1 -Type 'hdd' `
                -Medium $originalSata1 -NonRotational
            Set-BootOrder -Order $originalBootOrder
            Disable-SerialEvidence
            Copy-Item -LiteralPath $nvramBackup -Destination $nvramFile -Force
            $cleanupRestored = $true
        }
    }
    catch {
        $cleanupFailure = $_.Exception.Message
    }
}

$finalInformation = Get-VmInformation
$finalInformation | Set-Content `
    -LiteralPath (Join-Path $evidenceRoot 'vm-after.txt') -Encoding UTF8
$restoredConfiguration =
    $cleanupRestored -and
    (Get-MachineValue $finalInformation 'VMState') -eq 'poweroff' -and
    (Get-MachineValue $finalInformation 'firmware') -eq 'EFI64' -and
    (Get-MachineValue $finalInformation 'SecureBoot') -eq 'on' -and
    (Get-MachineValue $finalInformation 'uart1') -eq 'off' -and
    (Get-MachineValue $finalInformation 'uart2') -eq 'off' -and
    (Get-AttachmentValue $finalInformation 'IDE' 0) -eq $originalIde0 -and
    (Get-AttachmentValue $finalInformation 'IDE' 1) -eq $originalIde1 -and
    (Get-AttachmentValue $finalInformation 'SATA' 0) -eq $originalSata0 -and
    (Get-AttachmentValue $finalInformation 'SATA' 1) -eq $originalSata1
$passed =
    $null -eq $failure -and $null -eq $cleanupFailure -and
    $targetBootVerified -and
    $protectedHashBefore -eq $protectedHashAfter -and
    $restoredConfiguration
$result = [ordered]@{
    schemaVersion = 1
    result = $(if ($passed) { 'PASS' } else { 'FAIL' })
    vmName = $vmName
    productPath = 'VM-only linked product restore boundary'
    bootIsoPath = $bootIso
    bootIsoSha256 = $bootIsoHash
    protectedImageVdi = $protectedImageVdi
    protectedImageVdiSha256Before = $protectedHashBefore
    protectedImageVdiSha256After = $protectedHashAfter
    protectedImageVdiUnchanged = $protectedHashBefore -eq $protectedHashAfter
    workingImageVdi = $workingImageVdi
    restoredTargetVdi = $targetVdi
    restoreMarkerVerified = $restoreMarkerVerified
    restoreCompletionVerified = $restoreMarkerVerified -or $targetBootVerified
    restoreCompletionEvidence = $(
        if ($restoreMarkerVerified) { 'winpe-uart' }
        elseif ($targetBootVerified) { 'target-only-secure-boot' }
        else { 'none' }
    )
    targetOnlyUefiSecureBootVerified = $targetBootVerified
    targetGuestFields = $targetGuestFields
    targetProbeTransport = 'VirtualBox GuestControl with fixed password file'
    targetProbeStarted = $targetProbeStarted
    targetGuestShutdownObserved = $targetGuestShutdownObserved
    targetForcedStopAfterValidation = $targetForcedStopAfterValidation
    physicalDiskOrUsbUsed = $false
    nicDisabled = $true
    workerConfigurationRestored = $restoredConfiguration
    failure = $failure
    cleanupFailure = $cleanupFailure
    completedUtc = (Get-Date).ToUniversalTime().ToString('o')
}
[IO.File]::WriteAllText(
    $resultPath,
    ($result | ConvertTo-Json -Depth 6) + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false)
)
if (-not $passed) {
    throw "Product VSS restore VM test failed. Evidence: $evidenceRoot; failure=$failure; cleanup=$cleanupFailure"
}
Write-Host "Y-TEC Tsumugi Drive product VSS restore VM test: PASS"
Write-Host "Evidence: $evidenceRoot"
