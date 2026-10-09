# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $Package,
    [Parameter(Mandatory)][string] $Certificate,
    [Parameter(Mandatory)][string] $VCLibs,
    [Parameter(Mandatory)][string] $SdkVersion,
    [string] $ExpectedPublisher = 'CN=EdenXboxDev',
    [string] $ExpectedVersion,
    [string] $ExpectedThumbprint = $env:SIGNING_THUMBPRINT
)

$ErrorActionPreference = 'Stop'
$Package = (Resolve-Path -LiteralPath $Package).Path
$Certificate = (Resolve-Path -LiteralPath $Certificate).Path
$VCLibs = (Resolve-Path -LiteralPath $VCLibs).Path
$sdk = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin\$SdkVersion\x64"
$makeappx = Join-Path $sdk 'MakeAppx.exe'
$signtool = Join-Path $sdk 'SignTool.exe'
foreach ($tool in @($makeappx, $signtool)) {
    if (-not (Test-Path $tool)) { throw "Windows SDK tool is missing: $tool" }
}
if (-not (Test-Path (Join-Path $sdk 'dxil.dll'))) { throw "Pinned SDK $SdkVersion has no dxil.dll." }

$cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($Certificate)
if ($cert.Subject -ne $ExpectedPublisher) { throw "Certificate subject mismatch: $($cert.Subject)" }
if ($ExpectedThumbprint -and $cert.Thumbprint -ne ($ExpectedThumbprint -replace '\s', '')) {
    throw 'Exported public certificate does not match the pinned signing certificate thumbprint.'
}

$temporaryRoot = Join-Path $env:RUNNER_TEMP ('eden-appx-validation-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
try {
    $unpack = Join-Path $temporaryRoot 'package'
    & $makeappx unpack /p $Package /d $unpack /o
    if ($LASTEXITCODE -ne 0) { throw "MakeAppx unpack failed ($LASTEXITCODE)." }

    # MakeAppx has no standalone "validate" command. Successful unpack checks the APPX container;
    # SignTool below verifies the package signature and its block-map integrity.
    $signatureOutput = (& $signtool verify /pa /all /v $Package 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "APPX signature verification failed: $signatureOutput" }
    if ($signatureOutput -notmatch [regex]::Escape($ExpectedPublisher)) {
        throw 'APPX signature validation output does not identify the expected package publisher.'
    }
    $vclibsSignature = & $signtool verify /pa /all /v $VCLibs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $vclibsSignature -notmatch 'Microsoft Corporation') {
        throw "VCLibs package signature is invalid or is not Microsoft-signed: $vclibsSignature"
    }

    [xml]$manifest = Get-Content -LiteralPath (Join-Path $unpack 'AppxManifest.xml') -Raw
    $identity = $manifest.Package.Identity
    if ($identity.ProcessorArchitecture -ne 'x64' -or $identity.Publisher -ne $ExpectedPublisher) {
        throw "Manifest identity or architecture mismatch: arch=$($identity.ProcessorArchitecture), publisher=$($identity.Publisher)"
    }
    if ($ExpectedVersion -and $identity.Version -ne $ExpectedVersion) { throw "Manifest version mismatch: $($identity.Version)" }
    $codeGeneration = @($manifest.SelectNodes("//*[local-name()='Capability']") |
        Where-Object { $_.GetAttribute('Name') -eq 'codeGeneration' })
    if ($codeGeneration.Count -eq 0) {
        throw 'Manifest lost the required codeGeneration capability.'
    }
    $target = $manifest.Package.Dependencies.TargetDeviceFamily
    if ($target.Name -ne 'Windows.Universal') { throw "Unexpected device family target: $($target.Name)" }
    $framework = $manifest.Package.Dependencies.PackageDependency | Where-Object { $_.Name -eq 'Microsoft.VCLibs.140.00' }
    if (-not $framework) { throw 'Manifest no longer declares Microsoft.VCLibs.140.00.' }
    if ($framework.Publisher -ne 'CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US') {
        throw 'VCLibs publisher in the app manifest differs from Microsoft.'
    }
    if ([version]$framework.MinVersion -lt [version]'14.0.33519.0') {
        throw 'Manifest VCLibs minimum version is below the runtime used by this build.'
    }
    if ($manifest.Package.Applications.Application.Id -ne 'App' -or
        $manifest.Package.Applications.Application.Executable -ne 'eden-uwp.exe') {
        throw 'Unexpected package application identity or executable.'
    }

    foreach ($name in @('eden-uwp.exe','spirv_to_dxil.dll','dxil.dll')) {
        if (-not (Test-Path (Join-Path $unpack $name))) { throw "Required package runtime file is missing: $name" }
    }
    $peFiles = @('eden-uwp.exe','spirv_to_dxil.dll','dxil.dll')
    foreach ($name in $peFiles) {
        $stream = [IO.File]::OpenRead((Join-Path $unpack $name))
        try {
            $reader = [IO.BinaryReader]::new($stream)
            if ($reader.ReadUInt16() -ne 0x5A4D) { throw "$name is not a PE executable." }
            $stream.Position = 0x3C
            $peOffset = $reader.ReadInt32()
            $stream.Position = $peOffset
            if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) {
                throw "$name is not an x64 PE binary."
            }
        } finally { $stream.Dispose() }
    }
    $forbidden = Get-ChildItem $unpack -Recurse -File | Where-Object {
        $_.Name -match '^(prod\.keys|title\.keys|.*\.nca|.*\.(nsp|xci|rom|pfx))$'
    }
    if ($forbidden) { throw "Sensitive or copyrighted input found in package: $($forbidden.Name -join ', ')" }
    Write-Host "PASS: APPX structure, x64 manifest, codeGeneration, certificate thumbprint, package signature, Microsoft VCLibs signature, shader DLL/runtime presence, and SDK selection ($SdkVersion)."
    Write-Host "Publisher: $($identity.Publisher); version: $($identity.Version); certificate thumbprint: $($cert.Thumbprint)"
} finally {
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
}
