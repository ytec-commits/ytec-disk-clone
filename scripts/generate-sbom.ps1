param(
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Get-Content -LiteralPath (Join-Path $repoRoot 'sbom\project.json') -Raw |
    ConvertFrom-Json
$dependencyManifest = Get-Content -LiteralPath `
    (Join-Path $repoRoot 'third_party\dependencies.json') -Raw |
    ConvertFrom-Json

$rootPackageId = 'SPDXRef-Package-ytec-disk-clone'
$packages = @(
    [ordered]@{
        name                  = $project.name
        SPDXID                = $rootPackageId
        versionInfo           = $project.version
        downloadLocation      = $project.downloadLocation
        filesAnalyzed         = $false
        licenseConcluded      = $project.license
        licenseDeclared       = $project.license
        copyrightText         = $project.copyright
        supplier              = 'Organization: Y-TEC'
        primaryPackagePurpose = 'APPLICATION'
    }
)

$relationships = @(
    [ordered]@{
        spdxElementId      = 'SPDXRef-DOCUMENT'
        relationshipType  = 'DESCRIBES'
        relatedSpdxElement = $rootPackageId
    }
)

foreach ($dependency in $dependencyManifest.dependencies) {
    $safeName = [regex]::Replace($dependency.name, '[^A-Za-z0-9.-]', '-')
    $dependencyId = "SPDXRef-Package-$safeName"
    $packages += [ordered]@{
        name                  = $dependency.name
        SPDXID                = $dependencyId
        versionInfo           = $dependency.version
        downloadLocation      = $dependency.source
        filesAnalyzed         = $false
        licenseConcluded      = $dependency.license
        licenseDeclared       = $dependency.license
        copyrightText         = if (
            [string]::IsNullOrWhiteSpace($dependency.copyright)) {
            'NOASSERTION'
        } else {
            [string]$dependency.copyright
        }
        primaryPackagePurpose = if (
            [string]::IsNullOrWhiteSpace(
                $dependency.primaryPackagePurpose)) {
            'LIBRARY'
        } else {
            [string]$dependency.primaryPackagePurpose
        }
    }
    $relationships += [ordered]@{
        spdxElementId      = $rootPackageId
        relationshipType  = 'DEPENDS_ON'
        relatedSpdxElement = $dependencyId
    }
}

$document = [ordered]@{
    spdxVersion       = 'SPDX-2.3'
    dataLicense       = 'CC0-1.0'
    SPDXID            = 'SPDXRef-DOCUMENT'
    name              = "$($project.name)-$($project.version)"
    documentNamespace = $project.documentNamespace
    creationInfo      = [ordered]@{
        created  = $project.createdUtc
        creators = @(
            'Organization: Y-TEC'
            'Tool: scripts/generate-sbom.ps1'
        )
    }
    packages          = $packages
    relationships     = $relationships
}

$generated = $document | ConvertTo-Json -Depth 20
$outputPath = Join-Path $repoRoot 'SBOM.spdx.json'

if ($Check) {
    if (-not (Test-Path -LiteralPath $outputPath)) {
        throw 'SBOM.spdx.json がありません。generate-sbom.ps1 を実行してください。'
    }
    $existing = Get-Content -LiteralPath $outputPath -Raw |
        ConvertFrom-Json |
        ConvertTo-Json -Depth 20
    if ($existing.TrimEnd() -cne $generated.TrimEnd()) {
        throw 'SBOM.spdx.json が依存台帳と一致しません。generate-sbom.ps1 を再実行してください。'
    }
    Write-Output 'SBOM check: PASS'
    return
}

Set-Content -LiteralPath $outputPath -Value $generated -Encoding utf8
Write-Output "SBOM generated: $outputPath"
