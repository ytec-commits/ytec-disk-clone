[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $BootIsoPath,

    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string] $ExpectedBootIsoSha256,

    [ValidateRange(15, 180)]
    [int] $BootWaitSeconds = 25,

    [ValidateRange(10, 45)]
    [int] $MigrationTimeoutMinutes = 25,

    [ValidateRange(3, 15)]
    [int] $WindowsBootTimeoutMinutes = 8
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot '..\..'))
$vboxManage = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$vmName = 'YDC-Standalone-BootRepair-BIOS-x64'
$product = Join-Path $repoRoot (
    'out\build\msvc-x64-vm\src\WinPEApp\ytec-winpe-app.exe'
)
$fixture = Join-Path $repoRoot (
    'out\build\msvc-x64-vm-destructive\tests\' +
    'ytec-product-job-fixture-vm.exe'
)
$verifier = Join-Path $repoRoot (
    'out\build\msvc-x64-vm-destructive\tests\' +
    'ytec-product-mbr2gpt-verifier-vm.exe'
)
$guestRunner = Join-Path $repoRoot (
    'scripts\vm\Run-ProductMbr2GptValidation.cmd'
)
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$evidenceRoot = Join-Path $repoRoot (
    ".validation\evidence\product-mbr2gpt-vm\$timestamp"
)
$workingSource = Join-Path $evidenceRoot 'working-source-56GiB.vdi'
$targetVdi = Join-Path $evidenceRoot 'target-64GiB.vdi'
$markerVdi = Join-Path $evidenceRoot 'pass-marker-8MiB.vdi'
$markerRaw = Join-Path $evidenceRoot 'pass-marker-8MiB.raw'
$resultPath = Join-Path $evidenceRoot 'result.json'
$serialEvidencePath = Join-Path $evidenceRoot 'target-secure-boot-uart.txt'
$expectedMarker = 'YDC_PRODUCT_MBR2GPT_PASS_V1'
$expectedTargetMarker = 'YDC_TARGET_SECURE_BOOT_PASS_V1'
$sourceCapacityMb = 57344
$targetCapacityMb = 65536
$markerCapacityMb = 8

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
        -Operation 'Read VM information').Output
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

function Get-SataMedium {
    param(
        [Parameter(Mandatory)][string[]] $Information,
        [Parameter(Mandatory)][int] $Port
    )
    $key = '"SATA-' + $Port + '-0"'
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
            $canonical.StartsWith(
                $_, [StringComparison]::OrdinalIgnoreCase)
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

function Attach-Sata {
    param(
        [Parameter(Mandatory)][int] $Port,
        [Parameter(Mandatory)][ValidateSet('hdd', 'dvddrive')][string] $Type,
        [Parameter(Mandatory)][string] $Medium
    )
    $arguments = @(
        'storageattach', $vmName,
        '--storagectl', 'SATA',
        '--port', $Port.ToString(),
        '--device', '0',
        '--type', $Type,
        '--medium', $Medium
    )
    if ($Type -eq 'hdd') {
        $arguments += @('--nonrotational', 'on', '--discard', 'off')
    }
    $null = Invoke-VBox -Arguments $arguments -Operation "Attach SATA $Port"
}

function Detach-Sata {
    param(
        [Parameter(Mandatory)][int] $Port,
        [Parameter(Mandatory)][ValidateSet('hdd', 'dvddrive')][string] $Type,
        [switch] $BestEffort
    )
    $null = Invoke-VBox `
        -Arguments @(
            'storageattach', $vmName,
            '--storagectl', 'SATA',
            '--port', $Port.ToString(),
            '--device', '0',
            '--type', $Type,
            '--medium', 'none'
        ) `
        -Operation "Detach SATA $Port" `
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
        -Operation 'Set boot order'
}

function Set-SecureBoot {
    param([Parameter(Mandatory)][bool] $Enabled)
    $setting = if ($Enabled) { '--enable' } else { '--disable' }
    $null = Invoke-VBox `
        -Arguments @(
            'modifynvram', $vmName, 'secureboot', $setting
        ) `
        -Operation 'Set Secure Boot state'
}

function Enable-SerialEvidence {
    $null = Invoke-VBox `
        -Arguments @(
            'modifyvm', $vmName,
            '--uart1', '0x3F8', '4',
            '--uartmode1', 'file', $serialEvidencePath
        ) `
        -Operation 'Enable the target validation UART evidence channel'
}

function Disable-SerialEvidence {
    param([switch] $BestEffort)
    $null = Invoke-VBox `
        -Arguments @('modifyvm', $vmName, '--uart1', 'off') `
        -Operation 'Disable the target validation UART evidence channel' `
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

function Wait-ForPowerOff {
    param([Parameter(Mandatory)][DateTime] $Deadline)
    $index = 0
    while ((Get-Date) -lt $Deadline) {
        $state = Get-VmState
        if ($state -eq 'poweroff') {
            return
        }
        if ($state -ne 'running') {
            throw "Unexpected VM state while waiting for poweroff: $state"
        }
        $null = Save-Screenshot -Name (
            'migration-{0:D3}.png' -f $index
        )
        ++$index
        Start-Sleep -Seconds 10
    }
    throw 'The WinPE migration did not finish before the timeout.'
}

function Wait-ForTargetValidation {
    param(
        [Parameter(Mandatory)][DateTime] $Deadline
    )
    $index = 0
    while ((Get-Date) -lt $Deadline) {
        $state = Get-VmState
        if ($state -eq 'poweroff') {
            return
        }
        if ($state -notin @('running', 'starting', 'stopping')) {
            throw "Unexpected target VM state: $state"
        }
        $null = Save-Screenshot -Name (
            'target-secure-boot-{0:D3}.png' -f $index
        )
        ++$index
        Start-Sleep -Seconds 10
    }
    throw 'The target Windows validation did not finish before the timeout.'
}

function Wait-ForPowerOffState {
    param([Parameter(Mandatory)][DateTime] $Deadline)
    while ((Get-Date) -lt $Deadline) {
        if ((Get-VmState) -eq 'poweroff') {
            return $true
        }
        Start-Sleep -Seconds 5
    }
    return (Get-VmState) -eq 'poweroff'
}

function Read-Marker {
    $null = Invoke-VBox `
        -Arguments @(
            'clonemedium', 'disk', $markerVdi, $markerRaw,
            '--format', 'RAW'
        ) `
        -Operation 'Convert the dedicated marker VDI to RAW'
    $stream = [IO.File]::OpenRead($markerRaw)
    try {
        $buffer = [byte[]]::new(512)
        $read = $stream.Read($buffer, 0, $buffer.Length)
        if ($read -ne $buffer.Length) {
            throw 'Unable to read the complete marker sector.'
        }
        return [Text.Encoding]::ASCII.GetString($buffer).Trim([char]0)
    }
    finally {
        $stream.Dispose()
    }
}

foreach ($required in @(
    $vboxManage, $product, $fixture, $verifier, $guestRunner
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required validation file is missing: $required"
    }
}

$bootIso = Assert-AllowedFile -Path $BootIsoPath
$bootIsoHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $bootIso).Hash
if ($bootIsoHash -ine $ExpectedBootIsoSha256) {
    throw 'The WinPE boot ISO hash does not match the approved input.'
}

$running = (Invoke-VBox `
    -Arguments @('list', 'runningvms') `
    -Operation 'Check running VMs').Output
if ($running.Count -ne 0) {
    throw "Another VM is already running: $($running -join ', ')"
}

$originalInformation = Get-VmInformation
$originalState = Get-MachineValue $originalInformation 'VMState'
if ($originalState -notin @('poweroff', 'aborted') -or
    (Get-MachineValue $originalInformation 'firmware') -ne 'BIOS') {
    throw 'The fixed carrier VM must be stopped in Legacy BIOS mode.'
}
Assert-NetworkDisabled -Information $originalInformation
if ((Get-MachineValue $originalInformation 'uart1') -ne 'off') {
    throw 'The fixed carrier VM UART1 must be disabled before validation.'
}
foreach ($port in 1..3) {
    if ((Get-SataMedium $originalInformation $port) -ne 'none') {
        throw "The fixed carrier VM SATA port $port must be empty."
    }
}
$originalSource = Assert-AllowedFile -Path (
    Get-SataMedium -Information $originalInformation -Port 0
)
Assert-MediumCapacity -Path $originalSource -CapacityMb $sourceCapacityMb
$originalBootOrder = foreach ($index in 1..4) {
    Get-MachineValue -Information $originalInformation -Name "boot$index"
}
if ($originalBootOrder.Count -ne 4 -or
    @($originalBootOrder | Where-Object {
        [string]::IsNullOrWhiteSpace($_)
    }).Count -ne 0) {
    throw 'Unable to capture the original VM boot order.'
}

New-Item -ItemType Directory -Path $evidenceRoot -Force | Out-Null
$originalInformation | Set-Content `
    -LiteralPath (Join-Path $evidenceRoot 'vm-before.txt') `
    -Encoding UTF8

$configurationChanged = $false
$vmStarted = $false
$migrationPoweredOff = $false
$markerVerified = $false
$targetBootVerified = $false
$cleanupRestored = $false
$failure = $null
$cleanupFailure = $null
$sourceHashBefore = $null
$sourceHashAfter = $null
$originalSourceHashBefore = $null
$originalSourceHashAfter = $null
$guestBootOutput = @()

try {
    $originalSourceHashBefore = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $originalSource
    ).Hash
    $null = Invoke-VBox `
        -Arguments @(
            'clonemedium', 'disk', $originalSource, $workingSource,
            '--format', 'VDI', '--variant', 'Standard'
        ) `
        -Operation 'Clone the protected MBR source into a working VDI'
    Assert-MediumCapacity -Path $workingSource -CapacityMb $sourceCapacityMb
    $sourceHashBefore = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $workingSource
    ).Hash

    $null = Invoke-VBox `
        -Arguments @(
            'createmedium', 'disk', '--filename', $targetVdi,
            '--size', $targetCapacityMb.ToString(),
            '--format', 'VDI', '--variant', 'Standard'
        ) `
        -Operation 'Create the new RAW migration target VDI'
    $null = Invoke-VBox `
        -Arguments @(
            'createmedium', 'disk', '--filename', $markerVdi,
            '--size', $markerCapacityMb.ToString(),
            '--format', 'VDI', '--variant', 'Standard'
        ) `
        -Operation 'Create the new RAW PASS marker VDI'
    Assert-MediumCapacity -Path $targetVdi -CapacityMb $targetCapacityMb
    Assert-MediumCapacity -Path $markerVdi -CapacityMb $markerCapacityMb

    $configurationChanged = $true
    Detach-Sata -Port 0 -Type hdd
    Attach-Sata -Port 0 -Type hdd -Medium $workingSource
    Attach-Sata -Port 1 -Type hdd -Medium $targetVdi
    Attach-Sata -Port 2 -Type hdd -Medium $markerVdi
    Attach-Sata -Port 3 -Type dvddrive -Medium $bootIso
    Set-BootOrder -Order @('dvd', 'disk', 'none', 'none')

    $prepared = Get-VmInformation
    Assert-NetworkDisabled -Information $prepared
    $null = Invoke-VBox `
        -Arguments @('startvm', $vmName, '--type', 'headless') `
        -Operation 'Start the product migration WinPE'
    $vmStarted = $true
    foreach ($attempt in 1..8) {
        Start-Sleep -Milliseconds 750
        $null = Invoke-VBox `
            -Arguments @(
                'controlvm', $vmName,
                'keyboardputscancode', '39', 'b9'
            ) `
            -Operation 'Send the WinPE DVD boot key'
    }
    $bootDeadline = (Get-Date).AddSeconds($BootWaitSeconds)
    $startupScreenshotIndex = 0
    while ((Get-Date) -lt $bootDeadline) {
        if ((Get-VmState) -ne 'running') {
            throw 'The WinPE VM stopped during automatic migration startup.'
        }
        $null = Save-Screenshot -Name (
            'winpe-startup-{0:D2}.png' -f $startupScreenshotIndex
        )
        ++$startupScreenshotIndex
        Start-Sleep -Seconds 2
    }
    $null = Save-Screenshot -Name 'winpe-ready.png'
    Wait-ForPowerOff -Deadline (
        (Get-Date).AddMinutes($MigrationTimeoutMinutes)
    )
    $vmStarted = $false
    $migrationPoweredOff = $true

    Detach-Sata -Port 0 -Type hdd
    Detach-Sata -Port 1 -Type hdd
    Detach-Sata -Port 2 -Type hdd
    Detach-Sata -Port 3 -Type dvddrive

    $markerText = Read-Marker
    if (-not $markerText.StartsWith(
        $expectedMarker, [StringComparison]::Ordinal
    )) {
        throw 'The product PASS marker was not present after migration.'
    }
    $markerVerified = $true
    $sourceHashAfter = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $workingSource
    ).Hash

    $null = Invoke-VBox `
        -Arguments @(
            'modifyvm', $vmName,
            '--firmware', 'efi64'
        ) `
        -Operation 'Enable UEFI64 for target-only boot'
    Set-BootOrder -Order @('none', 'none', 'none', 'none')
    $nvramFile = Get-MachineValue `
        -Information (Get-VmInformation) `
        -Name 'NvramFile'
    if (-not (Test-Path -LiteralPath $nvramFile -PathType Leaf)) {
        # VirtualBox creates the per-VM UEFI variable store only after its
        # first EFI start.  Initialize it with every disk detached so this
        # prerequisite cannot write to either source or converted target.
        $null = Invoke-VBox `
            -Arguments @('startvm', $vmName, '--type', 'headless') `
            -Operation 'Initialize empty UEFI NVRAM without attached disks'
        $vmStarted = $true
        Start-Sleep -Seconds 3
        $null = Save-Screenshot -Name 'uefi-nvram-initialization.png'
        $null = Invoke-VBox `
            -Arguments @('controlvm', $vmName, 'poweroff') `
            -Operation 'Stop the diskless UEFI NVRAM initialization'
        Wait-ForPowerOff -Deadline ((Get-Date).AddMinutes(1))
        $vmStarted = $false
    }
    if (-not (Test-Path -LiteralPath $nvramFile -PathType Leaf)) {
        throw 'VirtualBox did not create the diskless UEFI NVRAM file.'
    }
    $null = Invoke-VBox `
        -Arguments @(
            'modifynvram', $vmName, 'enrollmssignatures'
        ) `
        -Operation 'Enroll VirtualBox Microsoft Secure Boot signatures'
    $null = Invoke-VBox `
        -Arguments @(
            'modifynvram', $vmName, 'enrollorclpk'
        ) `
        -Operation 'Enroll the VirtualBox UEFI platform key'
    Set-SecureBoot -Enabled $true
    Enable-SerialEvidence
    Attach-Sata -Port 0 -Type hdd -Medium $targetVdi
    Set-BootOrder -Order @('disk', 'none', 'none', 'none')

    $null = Invoke-VBox `
        -Arguments @('startvm', $vmName, '--type', 'headless') `
        -Operation 'Start the converted target by itself'
    $vmStarted = $true
    Wait-ForTargetValidation `
        -Deadline ((Get-Date).AddMinutes($WindowsBootTimeoutMinutes))
    $vmStarted = $false
    $null = Save-Screenshot -Name 'target-only-secure-boot.png'

    if (-not (Test-Path -LiteralPath $serialEvidencePath -PathType Leaf)) {
        throw 'The target UART evidence file was not created.'
    }
    $serialText = [Text.Encoding]::ASCII.GetString(
        [IO.File]::ReadAllBytes($serialEvidencePath)
    )
    $ansiPattern = [regex]::Escape([string][char]27) +
        '\[[0-9;=?]*[A-Za-z]'
    $cleanSerialText = [regex]::Replace($serialText, $ansiPattern, '')
    $uartMatches = [regex]::Matches(
        $cleanSerialText,
        'YDCUART=([^\r\n]*?);END'
    )
    if ($uartMatches.Count -eq 0) {
        throw 'The target UART evidence did not contain a validation record.'
    }
    $guestBootOutput = @(
        $uartMatches[$uartMatches.Count - 1].Groups[1].Value -split ';'
    )
    $guestBootOutput | Set-Content `
        -LiteralPath (Join-Path $evidenceRoot 'guest-boot-check.txt') `
        -Encoding UTF8
    foreach ($requiredLine in @(
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
        if ($guestBootOutput -notcontains $requiredLine) {
            throw "Target UART evidence is missing: $requiredLine"
        }
    }
    $targetBootVerified = $true
}
catch {
    $failure = $_.Exception.Message
}
finally {
    $cleanupErrors = [Collections.Generic.List[string]]::new()
    if ($vmStarted -and (Get-VmState) -ne 'poweroff') {
        try {
            $null = Save-Screenshot -Name 'failure-before-poweroff.png'
            $null = Invoke-VBox `
                -Arguments @('controlvm', $vmName, 'acpipowerbutton') `
                -Operation 'Request normal shutdown after validation failure' `
                -BestEffort
            $poweredOff = Wait-ForPowerOffState `
                -Deadline ((Get-Date).AddMinutes(2))
            if (-not $poweredOff) {
                throw (
                    'The VM did not stop normally; hard power-off was ' +
                    'intentionally not attempted.'
                )
            }
            $vmStarted = $false
        }
        catch {
            $cleanupErrors.Add($_.Exception.Message)
        }
    }
    if ($configurationChanged -and (Get-VmState) -eq 'poweroff') {
        try {
            Detach-Sata -Port 0 -Type hdd -BestEffort
            Detach-Sata -Port 1 -Type hdd -BestEffort
            Detach-Sata -Port 2 -Type hdd -BestEffort
            Detach-Sata -Port 3 -Type dvddrive -BestEffort
        }
        catch {
            $cleanupErrors.Add($_.Exception.Message)
        }
        try {
            Disable-SerialEvidence
        }
        catch {
            $cleanupErrors.Add($_.Exception.Message)
        }
        try {
            $firmwareBeforeRestore = Get-MachineValue `
                -Information (Get-VmInformation) -Name 'firmware'
            if ($firmwareBeforeRestore -ne 'BIOS') {
                $nvramBeforeRestore = Get-MachineValue `
                    -Information (Get-VmInformation) -Name 'NvramFile'
                if (Test-Path -LiteralPath $nvramBeforeRestore -PathType Leaf) {
                    Set-SecureBoot -Enabled $false
                }
            }
        }
        catch {
            $cleanupErrors.Add($_.Exception.Message)
        }
        try {
            $null = Invoke-VBox `
                -Arguments @('modifyvm', $vmName, '--firmware', 'bios') `
                -Operation 'Restore Legacy BIOS firmware'
        }
        catch {
            $cleanupErrors.Add($_.Exception.Message)
        }
        try {
            Attach-Sata -Port 0 -Type hdd -Medium $originalSource
        }
        catch {
            $cleanupErrors.Add($_.Exception.Message)
        }
        try {
            Set-BootOrder -Order $originalBootOrder
        }
        catch {
            $cleanupErrors.Add($_.Exception.Message)
        }
    }
    elseif ($configurationChanged) {
        $cleanupErrors.Add(
            'VM configuration restoration was skipped because the VM is not stopped.'
        )
    }
    try {
        $finalInformation = Get-VmInformation
        Assert-NetworkDisabled -Information $finalInformation
        $cleanupRestored =
            (Get-MachineValue $finalInformation 'VMState') -eq 'poweroff' -and
            (Get-MachineValue $finalInformation 'firmware') -eq 'BIOS' -and
            (Get-SataMedium $finalInformation 0) -eq $originalSource -and
            (Get-SataMedium $finalInformation 1) -eq 'none' -and
            (Get-SataMedium $finalInformation 2) -eq 'none' -and
            (Get-SataMedium $finalInformation 3) -eq 'none' -and
            (Get-MachineValue $finalInformation 'uart1') -eq 'off' -and
            (Invoke-VBox `
                -Arguments @('list', 'runningvms') `
                -Operation 'Check running VMs after cleanup').Output.Count -eq 0
    }
    catch {
        $cleanupErrors.Add($_.Exception.Message)
    }
    if ($cleanupErrors.Count -gt 0) {
        $cleanupFailure = $cleanupErrors -join ' | '
    }
}

try {
    if ($null -ne $sourceHashBefore -and
        (Test-Path -LiteralPath $workingSource -PathType Leaf)) {
        $sourceHashAfter = (
            Get-FileHash -Algorithm SHA256 -LiteralPath $workingSource
        ).Hash
    }
    $originalSourceHashAfter = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $originalSource
    ).Hash
}
catch {
    if ([string]::IsNullOrWhiteSpace($cleanupFailure)) {
        $cleanupFailure = $_.Exception.Message
    }
}

$sourceUnchanged =
    $null -ne $sourceHashBefore -and
    $sourceHashBefore -eq $sourceHashAfter
$originalSourceUnchanged =
    $null -ne $originalSourceHashBefore -and
    $originalSourceHashBefore -eq $originalSourceHashAfter
$passed =
    [string]::IsNullOrWhiteSpace($failure) -and
    [string]::IsNullOrWhiteSpace($cleanupFailure) -and
    $migrationPoweredOff -and
    $markerVerified -and
    $originalSourceUnchanged -and
    $targetBootVerified -and
    $cleanupRestored

$screenshots = @(
    Get-ChildItem -LiteralPath $evidenceRoot -Filter '*.png' -File `
        -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object {
            [ordered]@{
                name = $_.Name
                bytes = $_.Length
                sha256 = (
                    Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName
                ).Hash
            }
        }
)
$result = [ordered]@{
    schemaVersion = 2
    result = if ($passed) { 'PASS' } else { 'FAIL' }
    product = 'Y-TEC Tsumugi Drive'
    validation = 'product-mbr-to-gpt-target-only-secure-boot'
    vm = $vmName
    networkAdapters = 'none'
    physicalDiskOrUsbUsed = $false
    bootIso = [ordered]@{
        path = $bootIso
        sha256 = $bootIsoHash
    }
    executables = [ordered]@{
        productSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $product).Hash
        fixtureSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $fixture).Hash
        verifierSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $verifier).Hash
        destructiveVmHelpersOnly = $true
    }
    migration = [ordered]@{
        guestControlledPowerOff = $migrationPoweredOff
        passMarkerVerified = $markerVerified
        sourceLogicalIdentityVerifiedByProduct = $markerVerified
        sourceVdiContainerHashUnchanged = $sourceUnchanged
        sourceVdiContainerHashAdvisoryOnly = $true
        protectedBaselineVdiUnchanged = $originalSourceUnchanged
    }
    targetOnlyBoot = [ordered]@{
        firmware = 'UEFI64'
        secureBoot = $true
        sourceDetached = $true
        bootIsoDetached = $true
        validationTransport =
            'VM-only pre-staged startup probe + VirtualBox UART1 file'
        probePreStagedByVmOnlyVerifier = $true
        validationMediaAttached = $false
        serialValidationPassed = $targetBootVerified
        guestValidationPassed = $targetBootVerified
        guestOutput = $guestBootOutput
        chkdskScanPerformed = $false
        chkdskExitPassed = $null
        reagentcReadPerformed = $false
        reagentcReadPassed = $null
        criticalDiskNtfsEventReadPerformed = $false
        criticalDiskNtfsEventsReturned = $null
    }
    retainedEvidence = [ordered]@{
        workingSource = $workingSource
        target = $targetVdi
        marker = $markerVdi
        markerRaw = $markerRaw
        targetSerialEvidence = [ordered]@{
            path = $serialEvidencePath
            sha256 = if (Test-Path -LiteralPath $serialEvidencePath) {
                (Get-FileHash -Algorithm SHA256 `
                    -LiteralPath $serialEvidencePath).Hash
            } else {
                $null
            }
        }
        screenshots = $screenshots
    }
    cleanup = [ordered]@{
        originalVmConfigurationRestored = $cleanupRestored
        runningVmCount = @((Invoke-VBox `
            -Arguments @('list', 'runningvms') `
            -Operation 'Final running VM count' `
            -BestEffort).Output).Count
    }
    failure = $failure
    cleanupFailure = $cleanupFailure
    completedUtc = [DateTimeOffset]::UtcNow.ToString('o')
}
$result | ConvertTo-Json -Depth 10 | Set-Content `
    -LiteralPath $resultPath -Encoding UTF8

if (-not $passed) {
    throw "Product MBR2GPT VM validation failed. Evidence: $evidenceRoot"
}

Write-Host 'Product MBR2GPT + target-only Secure Boot VM validation: PASS'
Write-Host "Evidence: $evidenceRoot"
