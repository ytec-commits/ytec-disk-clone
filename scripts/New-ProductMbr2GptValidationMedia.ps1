[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $BaseWorkRoot,

    [Parameter(Mandatory)]
    [string] $OutputRoot,

    [ValidateSet('2011CA', '2023CA')]
    [string] $CertificateGeneration = '2023CA',

    [switch] $MsrDiagnostic
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$baseRoot = [IO.Path]::GetFullPath($BaseWorkRoot)
$outputRootValue = [IO.Path]::GetFullPath($OutputRoot)
$repositoryPrefix = $repoRoot.TrimEnd('\') + '\'
$localYtecPrefix = [IO.Path]::GetFullPath(
    (Join-Path $env:LOCALAPPDATA 'YTEC')
).TrimEnd('\') + '\'
if ($outputRootValue.StartsWith(
        $repositoryPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    -not $outputRootValue.StartsWith(
        $localYtecPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    (Test-Path -LiteralPath $outputRootValue)) {
    throw 'OutputRoot must be a new path under LocalAppData\YTEC and outside the repository.'
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator
    )) {
    throw 'Administrator rights are required to service the validation WIM.'
}

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
$runnerRelativePath = if ($MsrDiagnostic) {
    'scripts\vm\Run-ProductMsrDiagnostic.cmd'
}
else {
    'scripts\vm\Run-ProductMbr2GptValidation.cmd'
}
$runner = Join-Path $repoRoot $runnerRelativePath
$runnerName = if ($MsrDiagnostic) {
    'Run-ProductMsrDiagnostic.cmd'
}
else {
    'Run-ProductMbr2GptValidation.cmd'
}
$dism = Join-Path $env:SystemRoot 'System32\dism.exe'
$deploymentRoot = Join-Path ${env:ProgramFiles(x86)} (
    'Windows Kits\10\Assessment and Deployment Kit\Deployment Tools'
)
$oscdimgRoot = Join-Path $deploymentRoot 'amd64\Oscdimg'
$oscdimg = Join-Path $oscdimgRoot 'oscdimg.exe'
$etfsboot = Join-Path $oscdimgRoot 'etfsboot.com'
$efiBootImageName = if ($CertificateGeneration -eq '2023CA') {
    'efisys_EX.bin'
} else {
    'efisys.bin'
}
$efiBootImage = Join-Path $oscdimgRoot $efiBootImageName
$baseWorking = Join-Path $baseRoot 'working'
$baseWim = Join-Path $baseWorking 'media\sources\boot.wim'
foreach ($required in @(
    $product, $fixture, $verifier, $runner, $dism, $oscdimg,
    $etfsboot, $efiBootImage, $baseWim
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required validation input is missing: $required"
    }
}
if (-not (Test-Path -LiteralPath $baseWorking -PathType Container)) {
    throw 'The retained product WinPE working directory is missing.'
}

function Invoke-Native {
    param(
        [Parameter(Mandatory)][string] $Command,
        [Parameter(Mandatory)][string[]] $Arguments,
        [Parameter(Mandatory)][string] $Operation,
        [switch] $BestEffort
    )
    # Windows PowerShell 5.1 wraps native stderr as ErrorRecord objects.  Tools
    # such as oscdimg write normal progress to stderr, so Stop would otherwise
    # turn successful progress output into a terminating NativeCommandError.
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $Command @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($exitCode -ne 0 -and -not $BestEffort) {
        throw "$Operation failed (exit $exitCode):`n$($output -join "`n")"
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $output
    }
}

New-Item -ItemType Directory -Path $outputRootValue | Out-Null
$workingRoot = Join-Path $outputRootValue 'working'
Copy-Item -LiteralPath $baseWorking -Destination $workingRoot -Recurse
$mediaRoot = Join-Path $workingRoot 'media'
$mountRoot = Join-Path $workingRoot 'mount'
$bootWim = Join-Path $mediaRoot 'sources\boot.wim'
if (-not (Test-Path -LiteralPath $bootWim -PathType Leaf)) {
    throw 'The copied validation boot.wim is missing.'
}
if (@(Get-ChildItem -LiteralPath $mountRoot -Force).Count -ne 0) {
    throw 'The copied WIM mount directory is not empty.'
}

$sourceWimHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $bootWim).Hash
$mounted = $false
try {
    $null = Invoke-Native `
        -Command $dism `
        -Arguments @(
            '/Mount-Image', "/ImageFile:$bootWim", '/Index:1',
            "/MountDir:$mountRoot"
        ) `
        -Operation 'Mount the copied product WinPE WIM'
    $mounted = $true

    $payloadRoot = Join-Path $mountRoot 'YtecDiskClone'
    if (-not (Test-Path -LiteralPath $payloadRoot -PathType Container)) {
        throw 'The copied product payload directory is missing from the WIM.'
    }
    Copy-Item -LiteralPath $product `
        -Destination (Join-Path $payloadRoot 'ytec-winpe-app.exe') -Force
    Copy-Item -LiteralPath $fixture `
        -Destination (Join-Path $payloadRoot 'ytec-product-job-fixture-vm.exe')
    Copy-Item -LiteralPath $verifier `
        -Destination (Join-Path $payloadRoot 'ytec-product-mbr2gpt-verifier-vm.exe')
    Copy-Item -LiteralPath $runner `
        -Destination (Join-Path $payloadRoot $runnerName)

    $winpeshl = Join-Path $mountRoot 'Windows\System32\winpeshl.ini'
    $launchLines = @(
        '[LaunchApps]',
        '%SYSTEMROOT%\System32\wpeinit.exe',
        "%SYSTEMDRIVE%\YtecDiskClone\$runnerName"
    )
    [IO.File]::WriteAllLines(
        $winpeshl,
        $launchLines,
        [Text.ASCIIEncoding]::new()
    )

    $null = Invoke-Native `
        -Command $dism `
        -Arguments @('/Unmount-Image', "/MountDir:$mountRoot", '/Commit') `
        -Operation 'Commit the VM-only product MBR2GPT validation WIM'
    $mounted = $false
}
finally {
    if ($mounted) {
        $null = Invoke-Native `
            -Command $dism `
            -Arguments @('/Unmount-Image', "/MountDir:$mountRoot", '/Discard') `
            -Operation 'Discard the failed validation WIM mount' `
            -BestEffort
    }
}

$validationWimHash = (
    Get-FileHash -Algorithm SHA256 -LiteralPath $bootWim
).Hash
if ($validationWimHash -eq $sourceWimHash) {
    throw 'The validation WIM hash did not change after staging the runner.'
}

$isoName = if ($MsrDiagnostic) {
    "YDC-Product-MSR-Diagnostic-$CertificateGeneration.iso"
}
else {
    "YDC-Product-MBR2GPT-$CertificateGeneration.iso"
}
$isoPath = Join-Path $outputRootValue $isoName
$bootData = '-bootdata:2#p0,e,b{0}#pEF,e,b{1}' -f `
    $etfsboot, $efiBootImage
$null = Invoke-Native `
    -Command $oscdimg `
    -Arguments @(
        $bootData, '-u1', '-udfver102', $mediaRoot, $isoPath
    ) `
    -Operation 'Create the VM-only product MBR2GPT validation ISO'

$manifest = [ordered]@{
    schemaVersion = 1
    purpose = if ($MsrDiagnostic) {
        'VM-only product post-MBR2GPT MSR command diagnostic media'
    }
    else {
        'VM-only product MBR2GPT integration validation media'
    }
    product = 'Y-TEC Tsumugi Drive'
    certificateGeneration = $CertificateGeneration
    repositoryContainsMicrosoftPayload = $false
    baseProductWorkRoot = $baseRoot
    sourceWimSha256 = $sourceWimHash
    validationWimSha256 = $validationWimHash
    payload = @(
        foreach ($item in @($product, $fixture, $verifier, $runner)) {
            [ordered]@{
                path = $item
                bytes = (Get-Item -LiteralPath $item).Length
                sha256 = (
                    Get-FileHash -Algorithm SHA256 -LiteralPath $item
                ).Hash
            }
        }
    )
    iso = [ordered]@{
        path = $isoPath
        bytes = (Get-Item -LiteralPath $isoPath).Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $isoPath).Hash
    }
    generatedUtc = [DateTimeOffset]::UtcNow.ToString('o')
}
$manifestPath = Join-Path $outputRootValue 'manifest.json'
$manifest | ConvertTo-Json -Depth 8 | Set-Content `
    -LiteralPath $manifestPath -Encoding UTF8

[pscustomobject]@{
    IsoPath = $isoPath
    IsoSha256 = $manifest.iso.sha256
    Manifest = $manifestPath
} | Format-List
