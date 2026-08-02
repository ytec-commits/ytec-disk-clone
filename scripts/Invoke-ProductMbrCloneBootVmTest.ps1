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
    [int] $CloneTimeoutMinutes = 90,

    [ValidateRange(3, 20)]
    [int] $WindowsBootTimeoutMinutes = 10
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot '..\..'))
$vboxManage = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$vmName = 'YDC-WinPE-Product-Preflight'
$harness = Join-Path $repoRoot (
    'out\build\msvc-x64-vm-destructive\tests\' +
    'ytec-product-job-fixture-vm.exe'
)
$product = Join-Path $repoRoot (
    'out\build\msvc-x64-vm-destructive\src\WinPEApp\' +
    'ytec-winpe-app.exe'
)
$guestRunner = Join-Path $repoRoot (
    'scripts\vm\Run-ProductMbrCloneBootValidation.cmd'
)
$protectedSourceVdi = Join-Path $repoRoot (
    '.validation\vms\YDC-Standalone-BootRepair-BIOS-x64\' +
    'YDC-Standalone-BootRepair-BIOS-x64-56GiB.vdi'
)
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$evidenceRoot = Join-Path $repoRoot (
    ".validation\evidence\product-mbr-clone-boot-vm\$timestamp"
)
$workingSourceVdi = Join-Path $evidenceRoot 'working-source-56GiB.vdi'
$targetVdi = Join-Path $evidenceRoot 'cloned-target-64GiB.vdi'
$payloadViso = Join-Path $evidenceRoot 'mbr-clone-payload.viso'
$serialEvidencePath = Join-Path $evidenceRoot 'uart.txt'
$resultPath = Join-Path $evidenceRoot 'result.json'
$nvramBackup = Join-Path $evidenceRoot 'worker-nvram-before.bin'
$sourceCapacityMb = 57344
$targetCapacityMb = 65536
$expectedCloneMarker = 'YDC_PRODUCT_MBR_CLONE_PASS'
$expectedTargetMarker = 'YDC_TARGET_LEGACY_BIOS_PASS_V1'

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

function Set-Firmware {
    param([Parameter(Mandatory)][ValidateSet('BIOS', 'EFI64')][string] $Firmware)
    $value = if ($Firmware -eq 'BIOS') { 'bios' } else { 'efi64' }
    $null = Invoke-VBox `
        -Arguments @('modifyvm', $vmName, '--firmware', $value) `
        -Operation "Set worker firmware to $Firmware"
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

function Invoke-KeyboardRunCommand {
    param(
        [Parameter(Mandatory)][string] $Command,
        [Parameter(Mandatory)][string] $Operation
    )
    foreach ($arguments in @(
        @('controlvm', $vmName, 'keyboardputscancode', '01', '81'),
        @('controlvm', $vmName, 'keyboardputscancode',
            'e0', '5b', '13', '93', 'e0', 'db')
    )) {
        $result = Invoke-VBox `
            -Arguments $arguments `
            -Operation $Operation `
            -BestEffort
        if ($result.ExitCode -ne 0) {
            return $false
        }
        Start-Sleep -Milliseconds 700
    }
    foreach ($chunk in @(
        [regex]::Matches($Command, '.{1,48}') |
            ForEach-Object { $_.Value }
    )) {
        $result = Invoke-VBox `
            -Arguments @('controlvm', $vmName, 'keyboardputstring', $chunk) `
            -Operation $Operation `
            -BestEffort
        if ($result.ExitCode -ne 0) {
            return $false
        }
        Start-Sleep -Milliseconds 250
    }
    Start-Sleep -Seconds 1
    foreach ($attempt in 1..3) {
        $result = Invoke-VBox `
            -Arguments @(
                'controlvm', $vmName, 'keyboardputscancode', '1c', '9c'
            ) `
            -Operation $Operation `
            -BestEffort
        if ($result.ExitCode -eq 0) {
            Start-Sleep -Milliseconds 700
            $null = Invoke-VBox `
                -Arguments @(
                    'controlvm', $vmName,
                    'keyboardputscancode', '1c', '9c'
                ) `
                -Operation "$Operation (IME confirmation)" `
                -BestEffort
            Start-Sleep -Seconds 1
            return $true
        }
        Start-Sleep -Seconds 10
    }
    return $false
}

function Install-AndLaunchTargetValidation {
    param([Parameter(Mandatory)][string] $HexEncodedScript)
    $chunks = @(
        [regex]::Matches($HexEncodedScript, '.{1,160}') |
            ForEach-Object { $_.Value }
    )
    if ($chunks.Count -eq 0) {
        return $false
    }
    for ($index = 0; $index -lt $chunks.Count; ++$index) {
        $redirect = if ($index -eq 0) { '>' } else { '>>' }
        if (-not (Invoke-KeyboardRunCommand `
                -Command (
                    'cmd.exe /d /c echo ' + $chunks[$index] + $redirect +
                    'YdcValidation.hex'
                ) `
                -Operation 'Stage an encoded target validation chunk')) {
            return $false
        }
    }
    if (-not (Invoke-KeyboardRunCommand `
            -Command (
                'certutil.exe -f -decodehex YdcValidation.hex ' +
                'YdcValidation.ps1 12'
            ) `
            -Operation 'Decode the staged target validation script')) {
        return $false
    }
    return Invoke-KeyboardRunCommand `
        -Command (
            'powershell.exe -NoLogo -NoProfile -NonInteractive ' +
            '-ExecutionPolicy Bypass -File YdcValidation.ps1'
        ) `
        -Operation 'Launch the staged target validation script'
}

function Read-UartRecords {
    if (-not (Test-Path -LiteralPath $serialEvidencePath -PathType Leaf)) {
        return @()
    }
    $serialText = [Text.Encoding]::ASCII.GetString(
        [IO.File]::ReadAllBytes($serialEvidencePath)
    )
    $ansiPattern = [regex]::Escape([string][char]27) +
        '\[[0-9;=?]*[A-Za-z]'
    $clean = [regex]::Replace($serialText, $ansiPattern, '')
    return @([regex]::Matches($clean, 'YDCUART=([^\r\n]*?);END') |
        ForEach-Object { $_.Groups[1].Value })
}

function Write-VisoDescriptor {
    if ($harness.Contains("'") -or $product.Contains("'") -or
        $guestRunner.Contains("'")) {
        throw 'VISO payload path cannot contain a single quote.'
    }
    $marker = [Guid]::NewGuid().ToString()
    $line = "--iprt-iso-maker-file-marker-bourne-sh $marker " +
        "--file-mode=0444 --dir-mode=0555 " +
        "'/ytec-product-job-fixture-vm.exe=$harness' " +
        "'/ytec-winpe-app.exe=$product' " +
        "'/Run-ProductMbrCloneBootValidation.cmd=$guestRunner'"
    [IO.File]::WriteAllText(
        $payloadViso,
        $line + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false)
    )
}

foreach ($required in @(
    $vboxManage, $harness, $product, $guestRunner, $protectedSourceVdi
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
    (Get-MachineValue $originalInformation 'uart1') -ne 'off') {
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
$cloneMarkerVerified = $false
$winpeShutdownObserved = $false
$targetBootVerified = $false
$targetGuestFields = @()
$targetProbeStarted = $false

try {
    $protectedHashBefore = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $protectedSourceVdi
    ).Hash
    Write-VisoDescriptor
    $null = Invoke-VBox `
        -Arguments @(
            'clonemedium', 'disk', $protectedSourceVdi, $workingSourceVdi,
            '--format', 'VDI', '--variant', 'Standard'
        ) `
        -Operation 'Clone protected MBR Windows source VDI'
    $null = Invoke-VBox `
        -Arguments @(
            'createmedium', 'disk', '--filename', $targetVdi,
            '--size', $targetCapacityMb.ToString(),
            '--format', 'VDI', '--variant', 'Standard'
        ) `
        -Operation 'Create disposable 110GiB RAW clone target'
    Assert-MediumCapacity -Path $workingSourceVdi -CapacityMb $sourceCapacityMb
    Assert-MediumCapacity -Path $targetVdi -CapacityMb $targetCapacityMb

    $configurationChanged = $true
    Detach-Medium -Controller 'IDE' -Port 0 -Type 'dvddrive'
    Detach-Medium -Controller 'IDE' -Port 1 -Type 'dvddrive'
    Detach-Medium -Controller 'SATA' -Port 0 -Type 'hdd'
    Detach-Medium -Controller 'SATA' -Port 1 -Type 'hdd'
    Set-SecureBoot -Enabled $false
    Set-Firmware -Firmware 'BIOS'
    Attach-Medium -Controller 'IDE' -Port 0 -Type 'dvddrive' -Medium $bootIso
    Attach-Medium -Controller 'IDE' -Port 1 -Type 'dvddrive' -Medium $payloadViso
    Attach-Medium -Controller 'SATA' -Port 0 -Type 'hdd' `
        -Medium $workingSourceVdi -NonRotational
    Attach-Medium -Controller 'SATA' -Port 1 -Type 'hdd' `
        -Medium $targetVdi -NonRotational
    Set-BootOrder -Order @('dvd', 'none', 'none', 'none')
    Enable-SerialEvidence
    Assert-NetworkDisabled -Information (Get-VmInformation)

    $null = Invoke-VBox `
        -Arguments @('startvm', $vmName, '--type', 'headless') `
        -Operation 'Start product MBR clone WinPE'
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
    $deadline = $validationStartedAt.AddMinutes($CloneTimeoutMinutes)
    $screenIndex = 0
    while ((Get-Date) -lt $deadline) {
        $state = Get-VmState
        if ($state -eq 'poweroff') {
            break
        }
        if ($state -ne 'running') {
            throw "Unexpected VM state during MBR clone: $state"
        }
        if ($screenIndex % 6 -eq 0) {
            Write-Host (
                'MBR clone VM is running: {0:N1} minutes elapsed' -f
                ((Get-Date) - $validationStartedAt).TotalMinutes
            )
        }
        $null = Save-Screenshot -Name (
            'clone-progress-{0:D3}.png' -f $screenIndex
        )
        ++$screenIndex
        Start-Sleep -Seconds 10
    }
    if ((Get-VmState) -ne 'poweroff') {
        throw "MBR clone did not finish within $CloneTimeoutMinutes minutes."
    }
    $vmStarted = $false
    $winpeShutdownObserved = $true
    $records = Read-UartRecords
    $cloneMarkerVerified = @($records | Where-Object {
            $_ -eq $expectedCloneMarker
        }).Count -gt 0

    Detach-Medium -Controller 'IDE' -Port 0 -Type 'dvddrive'
    Detach-Medium -Controller 'IDE' -Port 1 -Type 'dvddrive'
    Detach-Medium -Controller 'SATA' -Port 0 -Type 'hdd'
    Detach-Medium -Controller 'SATA' -Port 1 -Type 'hdd'
    Attach-Medium -Controller 'SATA' -Port 0 -Type 'hdd' `
        -Medium $targetVdi -NonRotational
    Set-BootOrder -Order @('disk', 'none', 'none', 'none')

    $guestCheck = @'
$ErrorActionPreference='Stop'
$diskStyle='ERROR'
$isBoot=0
$isSystem=0
$partitionCount=0
$activeCount=0
$architecture64=0
$probeError=0
$shutdownScheduled=0
try {
    $disk=Get-Disk -Number 0
    $parts=@(Get-Partition -DiskNumber 0)
    $diskStyle=[string]$disk.PartitionStyle
    $isBoot=[int][bool]$disk.IsBoot
    $isSystem=[int][bool]$disk.IsSystem
    $partitionCount=$parts.Count
    $activeCount=@($parts | Where-Object { $_.IsActive }).Count
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
    $probeError -eq 0 -and $diskStyle -eq 'MBR' -and
    $isBoot -eq 1 -and $isSystem -eq 1 -and
    $activeCount -eq 1 -and $architecture64 -eq 1 -and
    $shutdownScheduled -eq 1
)
$marker=if($passed){'YDC_TARGET_LEGACY_BIOS_PASS_V1'}else{'YDC_TARGET_LEGACY_BIOS_FAIL_V1'}
$fields=@(
    $marker,('DiskStyle='+$diskStyle),('IsBoot='+$isBoot),
    ('IsSystem='+$isSystem),('PartitionCount='+$partitionCount),
    ('ActiveCount='+$activeCount),('Architecture64='+$architecture64),
    ('ProbeError='+$probeError),('ShutdownScheduled='+$shutdownScheduled)
)
$payload='YDCUART='+($fields -join ';')+';END'
& $env:ComSpec /d /c ('echo '+$payload+' >COM1') | Out-Null
Write-Output 'YDC_TARGET_PROBE_LAUNCHED'
'@.Trim()
    $utf8Bom = [Text.UTF8Encoding]::new($true)
    $guestScriptBytes = [byte[]](
        $utf8Bom.GetPreamble() + $utf8Bom.GetBytes($guestCheck)
    )
    $hexGuestCheck = [BitConverter]::ToString($guestScriptBytes).Replace('-', '')
    $null = Invoke-VBox `
        -Arguments @('startvm', $vmName, '--type', 'headless') `
        -Operation 'Start cloned target only with Legacy BIOS'
    $vmStarted = $true
    $targetDeadline = (Get-Date).AddMinutes($WindowsBootTimeoutMinutes)
    $nextInjection = (Get-Date).AddSeconds(60)
    $targetScreen = 0
    while ((Get-Date) -lt $targetDeadline) {
        $state = Get-VmState
        if ($state -eq 'poweroff') {
            break
        }
        if ($state -ne 'running') {
            throw "Unexpected cloned target VM state: $state"
        }
        $null = Save-Screenshot -Name (
            'target-legacy-bios-{0:D3}.png' -f $targetScreen
        )
        ++$targetScreen
        if ((Get-Date) -ge $nextInjection) {
            $targetProbeStarted =
                (Install-AndLaunchTargetValidation `
                    -HexEncodedScript $hexGuestCheck) -or
                $targetProbeStarted
            $nextInjection = (Get-Date).AddSeconds(60)
        }
        Start-Sleep -Seconds 10
    }
    if ((Get-VmState) -ne 'poweroff') {
        throw 'Cloned target did not complete validation and shutdown.'
    }
    $vmStarted = $false
    $records = Read-UartRecords
    $targetRecord = @($records | Where-Object {
        $_.StartsWith($expectedTargetMarker, [StringComparison]::Ordinal)
    }) | Select-Object -Last 1
    if ([string]::IsNullOrWhiteSpace($targetRecord)) {
        throw 'Cloned target UART validation PASS record is missing.'
    }
    $targetGuestFields = @($targetRecord -split ';')
    foreach ($requiredField in @(
        $expectedTargetMarker,
        'DiskStyle=MBR',
        'IsBoot=1',
        'IsSystem=1',
        'ActiveCount=1',
        'Architecture64=1',
        'ProbeError=0',
        'ShutdownScheduled=1'
    )) {
        if ($targetGuestFields -notcontains $requiredField) {
            throw "Cloned target field missing: $requiredField"
        }
    }
    $targetBootVerified = $true
    $protectedHashAfter = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $protectedSourceVdi
    ).Hash
    if ($protectedHashBefore -ine $protectedHashAfter) {
        throw 'The protected MBR Windows source VDI changed.'
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
            Set-Firmware -Firmware 'EFI64'
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
    (Get-AttachmentValue $finalInformation 'IDE' 0) -eq $originalIde0 -and
    (Get-AttachmentValue $finalInformation 'IDE' 1) -eq $originalIde1 -and
    (Get-AttachmentValue $finalInformation 'SATA' 0) -eq $originalSata0 -and
    (Get-AttachmentValue $finalInformation 'SATA' 1) -eq $originalSata1
$passed =
    $null -eq $failure -and $null -eq $cleanupFailure -and
    $winpeShutdownObserved -and $targetBootVerified -and
    $protectedHashBefore -eq $protectedHashAfter -and
    $restoredConfiguration
$result = [ordered]@{
    schemaVersion = 1
    result = $(if ($passed) { 'PASS' } else { 'FAIL' })
    vmName = $vmName
    productPath = 'Product clone job execution with boot finalization'
    bootIsoPath = $bootIso
    bootIsoSha256 = $bootIsoHash
    protectedSourceVdi = $protectedSourceVdi
    protectedSourceVdiSha256Before = $protectedHashBefore
    protectedSourceVdiSha256After = $protectedHashAfter
    protectedSourceVdiUnchanged = $protectedHashBefore -eq $protectedHashAfter
    workingSourceVdi = $workingSourceVdi
    clonedTargetVdi = $targetVdi
    winpeShutdownObserved = $winpeShutdownObserved
    cloneMarkerVerified = $cloneMarkerVerified
    targetOnlyLegacyBiosVerified = $targetBootVerified
    targetGuestFields = $targetGuestFields
    targetProbeTransport = 'VirtualBox keyboard staging with UART result'
    targetProbeStarted = $targetProbeStarted
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
    throw "Product MBR clone boot VM test failed. Evidence: $evidenceRoot; failure=$failure; cleanup=$cleanupFailure"
}
Write-Host "Y-TEC Tsumugi Drive product MBR clone boot VM test: PASS"
Write-Host "Evidence: $evidenceRoot"
