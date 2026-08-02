param(
    [Parameter(Mandatory)]
    [string] $BaseProductMediaRoot,

    [Parameter(Mandatory)]
    [string] $OutputRoot,

    [Parameter(Mandatory)]
    [ValidateSet('VssRestore', 'GptClone', 'MbrClone')]
    [string] $Profile,

    [switch] $BuildMedia
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

function Assert-RegularNonReparseFile {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $Description
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description must not be a reparse point: $Path"
    }
    return $item
}

function Assert-NoReparsePointInTree {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $Description
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Description is missing: $Path"
    }
    $entry = Get-Item -LiteralPath $Path -Force
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description root must not be a reparse point: $Path"
    }
    $reparse = Get-ChildItem -LiteralPath $Path -Recurse -Force |
        Where-Object {
            ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
        } | Select-Object -First 1
    if ($null -ne $reparse) {
        throw "$Description contains a reparse point: $($reparse.FullName)"
    }
}

function Assert-ExternalNewOutputPath {
    param(
        [Parameter(Mandatory)][string] $RepositoryRoot,
        [Parameter(Mandatory)][string] $CandidatePath
    )
    $repository = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')
    $output = [IO.Path]::GetFullPath($CandidatePath).TrimEnd('\')
    if ($output.Equals($repository, [StringComparison]::OrdinalIgnoreCase) -or
        $output.StartsWith(
            $repository + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Microsoft WinPE output must remain outside the repository.'
    }
    $root = [IO.Path]::GetPathRoot($output).TrimEnd('\')
    if ([string]::IsNullOrWhiteSpace($root) -or
        $output.Equals($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'A drive root is not an allowed output directory.'
    }
    if (Test-Path -LiteralPath $output) {
        throw "The output directory already exists: $output"
    }
    $ancestor = Split-Path -Parent $output
    while (-not (Test-Path -LiteralPath $ancestor)) {
        $next = Split-Path -Parent $ancestor
        if ([string]::IsNullOrWhiteSpace($next) -or $next -eq $ancestor) {
            throw 'An existing output ancestor could not be resolved.'
        }
        $ancestor = $next
    }
    while (-not [string]::IsNullOrWhiteSpace($ancestor)) {
        $item = Get-Item -LiteralPath $ancestor -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "An output ancestor is a reparse point: $ancestor"
        }
        $next = Split-Path -Parent $ancestor
        if ([string]::IsNullOrWhiteSpace($next) -or $next -eq $ancestor) {
            break
        }
        $ancestor = $next
    }
    return $output
}

function Assert-MicrosoftSignature {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $Description
    )
    $null = Assert-RegularNonReparseFile -Path $Path -Description $Description
    $signature = Get-AuthenticodeSignature -FilePath $Path
    if ($signature.Status -ne
            [Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch
            '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw "$Description does not have a valid Microsoft signature: $Path"
    }
}

function Assert-Amd64Pe32Plus {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $Description
    )
    $item = Assert-RegularNonReparseFile -Path $Path -Description $Description
    if ($item.Length -lt 512 -or $item.Length -gt 64MB) {
        throw "$Description has an unexpected file size."
    }
    $stream = [IO.File]::OpenRead($item.FullName)
    try {
        $header = [byte[]]::new([Math]::Min(4096, [int]$item.Length))
        if ($stream.Read($header, 0, $header.Length) -ne $header.Length -or
            $header[0] -ne 0x4D -or $header[1] -ne 0x5A) {
            throw "$Description does not have a complete DOS header."
        }
        $peOffset = [BitConverter]::ToInt32($header, 0x3C)
        if ($peOffset -lt 0x40 -or $peOffset + 26 -gt $header.Length -or
            $header[$peOffset] -ne 0x50 -or
            $header[$peOffset + 1] -ne 0x45 -or
            [BitConverter]::ToUInt16($header, $peOffset + 4) -ne 0x8664 -or
            [BitConverter]::ToUInt16($header, $peOffset + 24) -ne 0x020B) {
            throw "$Description must be an AMD64 PE32+ executable."
        }
    }
    finally {
        $stream.Dispose()
    }
    return [ordered]@{
        path = $item.FullName
        length = $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName `
            -Algorithm SHA256).Hash
    }
}

function Invoke-CheckedNative {
    param(
        [Parameter(Mandatory)][string] $Command,
        [Parameter(Mandatory)][string[]] $Arguments,
        [Parameter(Mandatory)][string] $Operation
    )
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Command @Arguments
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -ne 0) {
        throw "$Operation failed with exit code $exitCode."
    }
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$baseRoot = [IO.Path]::GetFullPath($BaseProductMediaRoot)
$outputFullPath = Assert-ExternalNewOutputPath `
    -RepositoryRoot $repoRoot -CandidatePath $OutputRoot
$repoPrefix = $repoRoot.TrimEnd('\') + '\'
if ($baseRoot.Equals($repoRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $baseRoot.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The verified base media must remain outside the repository.'
}
Assert-NoReparsePointInTree -Path $baseRoot -Description 'Base product media'

$baseManifestPath = Join-Path $baseRoot 'winpe-app-media-manifest.json'
$baseMediaRoot = Join-Path $baseRoot 'working\media'
$baseBootWim = Join-Path $baseMediaRoot 'sources\boot.wim'
$null = Assert-RegularNonReparseFile -Path $baseManifestPath `
    -Description 'Base media manifest'
$null = Assert-RegularNonReparseFile -Path $baseBootWim `
    -Description 'Base boot.wim'
$baseManifest = Get-Content -LiteralPath $baseManifestPath -Raw |
    ConvertFrom-Json
if ($baseManifest.schemaVersion -ne 1 -or
    $baseManifest.purpose -ne
        'Y-TEC Tsumugi Drive WinPEApp validation media' -or
    $baseManifest.repositoryContainsMicrosoftPayload -ne $false -or
    $baseManifest.certificateGeneration -ne '2023CA' -or
    $baseManifest.adkVersion -ne '10.1.26100.2454' -or
    $baseManifest.dismVersion -ne '10.0.26100.8972' -or
    $baseManifest.servicingUpdate -ne 'KB5101684') {
    throw 'The base media manifest does not match the fixed 2023 CA profile.'
}
$baseWimHash = (Get-FileHash -LiteralPath $baseBootWim `
    -Algorithm SHA256).Hash
if ($baseWimHash -ne [string]$baseManifest.stagedWimAfter.sha256 -or
    (Get-Item -LiteralPath $baseBootWim).Length -ne
        [long]$baseManifest.stagedWimAfter.length) {
    throw 'The base boot.wim does not match its manifest.'
}

$diagnostic = Join-Path $repoRoot (
    'out\build\msvc-x64\src\MediaBuilder\ytec-winpe-environment.exe'
)
$null = Assert-Amd64Pe32Plus -Path $diagnostic `
    -Description 'WinPE environment diagnostic'
$diagnosticStartInfo = [Diagnostics.ProcessStartInfo]::new()
$diagnosticStartInfo.FileName = $diagnostic
# Windows PowerShell 5.1 runs on .NET Framework, where ProcessStartInfo does
# not expose ArgumentList.  This fixed, metacharacter-free argument is safe to
# pass through Arguments and keeps the elevated builder compatible with both
# Windows PowerShell and PowerShell 7.
$diagnosticStartInfo.Arguments = '--json'
$diagnosticStartInfo.UseShellExecute = $false
$diagnosticStartInfo.RedirectStandardOutput = $true
$diagnosticStartInfo.RedirectStandardError = $true
$diagnosticStartInfo.CreateNoWindow = $true
$diagnosticProcess = [Diagnostics.Process]::Start($diagnosticStartInfo)
$diagnosticOutput = $diagnosticProcess.StandardOutput.ReadToEnd()
$diagnosticError = $diagnosticProcess.StandardError.ReadToEnd()
$diagnosticProcess.WaitForExit()
if ($diagnosticProcess.ExitCode -ne 0) {
    throw "WinPE environment diagnostic failed: $diagnosticError"
}

# The diagnostic's exit code is the authoritative pass/fail boundary.  Derive
# the already-validated ADK root from the signed base-media manifest instead of
# reparsing native UTF-8 output in an elevated PowerShell host.  Some elevated
# hosts normalize captured line separators and can make otherwise valid JSON
# look like it contains literal "\n" separators.
$manifestSourceWim = [IO.Path]::GetFullPath(
    [string]$baseManifest.sourceWim.path)
$adkRoot = $manifestSourceWim
for ($level = 0; $level -lt 4; ++$level) {
    $adkRoot = Split-Path -Parent $adkRoot
}
if ([string]::IsNullOrWhiteSpace($adkRoot)) {
    throw 'The ADK root could not be derived from the base-media manifest.'
}
$adkRoot = [IO.Path]::GetFullPath($adkRoot)
$oscdimgRoot = Join-Path $adkRoot 'Deployment Tools\amd64\Oscdimg'
$dism = Join-Path $adkRoot 'Deployment Tools\amd64\DISM\dism.exe'
$oscdimg = Join-Path $oscdimgRoot 'oscdimg.exe'
$etfsboot = Join-Path $oscdimgRoot 'etfsboot.com'
$efiBootImage = Join-Path $oscdimgRoot 'efisys_EX.bin'
Assert-MicrosoftSignature -Path $dism -Description 'ADK DISM'
Assert-MicrosoftSignature -Path $oscdimg -Description 'ADK Oscdimg'
$null = Assert-RegularNonReparseFile -Path $etfsboot `
    -Description 'ADK BIOS boot image'
$null = Assert-RegularNonReparseFile -Path $efiBootImage `
    -Description 'ADK 2023 CA UEFI boot image'

$fixture = Join-Path $repoRoot (
    'out\build\msvc-x64-vm-destructive\tests\' +
    'ytec-product-job-fixture-vm.exe'
)
$product = Join-Path $repoRoot (
    'out\build\msvc-x64-vm-destructive\src\WinPEApp\' +
    'ytec-winpe-app.exe'
)
$vssHarness = Join-Path $repoRoot (
    'out\build\msvc-x64-vm-destructive\tests\' +
    'ytec-product-vss-restore-vm.exe'
)
$runner = switch ($Profile) {
    VssRestore {
        Join-Path $repoRoot 'scripts\vm\Run-ProductVssRestoreValidation.cmd'
    }
    GptClone {
        Join-Path $repoRoot 'scripts\vm\Run-ProductGptCloneBootValidation.cmd'
    }
    MbrClone {
        Join-Path $repoRoot 'scripts\vm\Run-ProductMbrCloneBootValidation.cmd'
    }
}
$payloads = [Collections.Generic.List[object]]::new()
if ($Profile -eq 'VssRestore') {
    $payloads.Add([pscustomobject]@{
        Source = $vssHarness
        Name = 'ytec-product-vss-restore-vm.exe'
        Description = 'Product VSS restore VM harness'
    })
}
else {
    $payloads.Add([pscustomobject]@{
        Source = $fixture
        Name = 'ytec-product-job-fixture-vm.exe'
        Description = 'Product job fixture VM harness'
    })
    $payloads.Add([pscustomobject]@{
        Source = $product
        Name = 'ytec-winpe-app.exe'
        Description = 'Product WinPE CLI'
    })
}
$payloadReports = @(
    foreach ($payload in $payloads) {
        Assert-Amd64Pe32Plus -Path $payload.Source `
            -Description $payload.Description
    }
)
$runnerItem = Assert-RegularNonReparseFile -Path $runner `
    -Description 'Fixed VM launch script'

$preflight = [ordered]@{
    schemaVersion = 1
    purpose = 'Product boot finalization VM-only validation media'
    profile = $Profile
    baseProductMediaRoot = $baseRoot
    baseBootWimSha256 = $baseWimHash
    outputRoot = $outputFullPath
    outputExists = $false
    certificateGeneration = '2023CA'
    payloads = $payloadReports
    launchScriptSha256 = (Get-FileHash -LiteralPath $runnerItem.FullName `
        -Algorithm SHA256).Hash
    buildRequested = [bool]$BuildMedia
    administrator = Test-IsAdministrator
}
if (-not $BuildMedia) {
    Write-Output ('PRODUCT_BOOT_MEDIA_PREFLIGHT_PASS=' +
        ($preflight | ConvertTo-Json -Depth 7 -Compress))
    return
}
if (-not $preflight.administrator) {
    throw 'Administrator rights are required to mount and commit boot.wim.'
}
Write-Output "PRODUCT_BOOT_MEDIA_PROGRESS=$Profile|5|preflight"

$workingRoot = Join-Path $outputFullPath 'working'
$mediaRoot = Join-Path $workingRoot 'media'
$mountRoot = Join-Path $workingRoot 'mount'
$bootWim = Join-Path $mediaRoot 'sources\boot.wim'
$isoPath = Join-Path $outputFullPath (
    "YDC-Product-$Profile-VMOnly-amd64-2023CA.iso"
)
$manifestPath = Join-Path $outputFullPath `
    'product-boot-validation-media-manifest.json'
New-Item -ItemType Directory -Path $outputFullPath | Out-Null
New-Item -ItemType Directory -Path $workingRoot | Out-Null
Write-Output "PRODUCT_BOOT_MEDIA_PROGRESS=$Profile|12|created-working-area"
Copy-Item -LiteralPath $baseMediaRoot -Destination $mediaRoot -Recurse
Write-Output "PRODUCT_BOOT_MEDIA_PROGRESS=$Profile|24|copied-base-media"
New-Item -ItemType Directory -Path $mountRoot | Out-Null
(Get-Item -LiteralPath $bootWim).IsReadOnly = $false
if ((Get-FileHash -LiteralPath $bootWim -Algorithm SHA256).Hash -ne
    $baseWimHash) {
    throw 'The copied base boot.wim hash changed before servicing.'
}
Write-Output "PRODUCT_BOOT_MEDIA_PROGRESS=$Profile|32|verified-base-wim"

$mounted = $false
$changedFiles = @()
try {
    Write-Output "PRODUCT_BOOT_MEDIA_PROGRESS=$Profile|38|mounting-wim"
    Invoke-CheckedNative -Command $dism -Arguments @(
        '/Mount-Image', "/ImageFile:$bootWim", '/Index:1',
        "/MountDir:$mountRoot"
    ) -Operation 'Mount VM-only product validation boot.wim'
    $mounted = $true
    Write-Output "PRODUCT_BOOT_MEDIA_PROGRESS=$Profile|48|mounted-wim"

    $payloadRoot = Join-Path $mountRoot 'YtecDiskClone'
    $mountedLaunch = Join-Path $payloadRoot 'launch.cmd'
    $mountedWinpeShell = Join-Path $mountRoot `
        'Windows\System32\winpeshl.ini'
    if (-not (Test-Path -LiteralPath $payloadRoot -PathType Container) -or
        -not (Test-Path -LiteralPath $mountedLaunch -PathType Leaf) -or
        -not (Test-Path -LiteralPath $mountedWinpeShell -PathType Leaf)) {
        throw 'The verified base WIM product payload is incomplete.'
    }
    foreach ($payload in $payloads) {
        $destination = Join-Path $payloadRoot $payload.Name
        if (Test-Path -LiteralPath $destination -PathType Leaf) {
            $baseRecord = @($baseManifest.addedFiles | Where-Object {
                $_.relativePath -eq "YtecDiskClone\$($payload.Name)"
            })
            if ($baseRecord.Count -ne 1 -or
                (Get-FileHash -LiteralPath $destination `
                    -Algorithm SHA256).Hash -ne
                    [string]$baseRecord[0].sha256) {
                throw "An existing WIM payload is not the manifest-recorded base: $($payload.Name)"
            }
        }
        Copy-Item -LiteralPath $payload.Source -Destination $destination -Force
    }
    Copy-Item -LiteralPath $runnerItem.FullName `
        -Destination $mountedLaunch -Force
    @(
        '[LaunchApps]',
        '%SYSTEMROOT%\System32\wpeinit.exe',
        '%SYSTEMROOT%\System32\cmd.exe, /c %SYSTEMDRIVE%\YtecDiskClone\launch.cmd'
    ) | Set-Content -LiteralPath $mountedWinpeShell -Encoding ascii

    $changedFiles = @(
        foreach ($path in @(
            @($payloads | ForEach-Object {
                Join-Path $payloadRoot $_.Name
            }) + @($mountedLaunch, $mountedWinpeShell)
        )) {
            [ordered]@{
                relativePath = $path.Substring($mountRoot.Length).TrimStart('\')
                length = (Get-Item -LiteralPath $path).Length
                sha256 = (Get-FileHash -LiteralPath $path `
                    -Algorithm SHA256).Hash
            }
        }
    )
    foreach ($payload in $payloads) {
        $destination = Join-Path $payloadRoot $payload.Name
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $payload.Source -Algorithm SHA256).Hash) {
            throw "A staged VM payload hash mismatch occurred: $($payload.Name)"
        }
    }
    Write-Output "PRODUCT_BOOT_MEDIA_PROGRESS=$Profile|62|verified-payload"

    Invoke-CheckedNative -Command $dism -Arguments @(
        '/Unmount-Image', "/MountDir:$mountRoot", '/Commit', '/CheckIntegrity'
    ) -Operation 'Commit VM-only product validation boot.wim'
    $mounted = $false
    Write-Output "PRODUCT_BOOT_MEDIA_PROGRESS=$Profile|76|committed-wim"
}
catch {
    if ($mounted) {
        & $dism '/Unmount-Image' "/MountDir:$mountRoot" '/Discard'
    }
    throw
}

$stagedWimHash = (Get-FileHash -LiteralPath $bootWim `
    -Algorithm SHA256).Hash
if ($stagedWimHash -eq $baseWimHash) {
    throw 'The VM-only payload did not change boot.wim.'
}
$bootData = '-bootdata:2#p0,e,b{0}#pEF,e,b{1}' -f `
    $etfsboot, $efiBootImage
Invoke-CheckedNative -Command $oscdimg -Arguments @(
    $bootData, '-u1', '-udfver102', $mediaRoot, $isoPath
) -Operation 'Create VM-only product validation ISO'
Write-Output "PRODUCT_BOOT_MEDIA_PROGRESS=$Profile|90|generated-iso"
$isoItem = Assert-RegularNonReparseFile -Path $isoPath `
    -Description 'VM-only product validation ISO'
$manifest = [ordered]@{
    schemaVersion = 1
    purpose = 'Product boot finalization VM-only validation media'
    profile = $Profile
    vmOnly = $true
    physicalDiskOrUsbPermitted = $false
    repositoryContainsMicrosoftPayload = $false
    certificateGeneration = '2023CA'
    baseProductMediaRoot = $baseRoot
    baseBootWimSha256 = $baseWimHash
    stagedBootWimSha256 = $stagedWimHash
    changedFiles = $changedFiles
    iso = [ordered]@{
        path = $isoItem.FullName
        length = $isoItem.Length
        sha256 = (Get-FileHash -LiteralPath $isoItem.FullName `
            -Algorithm SHA256).Hash
    }
    retainedWorkRoot = $outputFullPath
    generatedUtc = (Get-Date).ToUniversalTime().ToString('o')
}
[IO.File]::WriteAllText(
    $manifestPath,
    ($manifest | ConvertTo-Json -Depth 8) + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false)
)
Write-Output "PRODUCT_BOOT_MEDIA_PROGRESS=$Profile|100|completed"
Write-Output ('PRODUCT_BOOT_MEDIA_BUILD_PASS=' +
    ($manifest | ConvertTo-Json -Depth 8 -Compress))
