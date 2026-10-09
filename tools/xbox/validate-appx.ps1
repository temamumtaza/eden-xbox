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

    Add-Type -AssemblyName System.Security.Cryptography.Pkcs
    Add-Type -AssemblyName System.Formats.Asn1
    $signaturePath = Join-Path $unpack 'AppxSignature.p7x'
    $blockMapPath = Join-Path $unpack 'AppxBlockMap.xml'
    $signatureBytes = [IO.File]::ReadAllBytes($signaturePath)
    if ($signatureBytes.Length -le 4 -or [Text.Encoding]::ASCII.GetString($signatureBytes, 0, 4) -ne 'PKCX') {
        throw 'APPX signature has no valid PKCX header.'
    }
    $cmsBytes = [byte[]]::new($signatureBytes.Length - 4)
    [Array]::Copy($signatureBytes, 4, $cmsBytes, 0, $cmsBytes.Length)
    $cms = [System.Security.Cryptography.Pkcs.SignedCms]::new()
    $cms.Decode($cmsBytes)
    $cms.CheckSignature($true)
    if ($cms.ContentInfo.ContentType.Value -ne '1.3.6.1.4.1.311.2.1.4') {
        throw 'APPX signature does not contain SPC indirect data.'
    }
    if ($cms.SignerInfos.Count -ne 1) { throw 'APPX signature must contain exactly one signer.' }
    $signerCertificate = $cms.SignerInfos[0].Certificate
    if (-not $signerCertificate -or $signerCertificate.Subject -ne $ExpectedPublisher) {
        throw 'APPX signature signer subject does not match the expected publisher.'
    }
    if ($ExpectedThumbprint -and $signerCertificate.Thumbprint -ne ($ExpectedThumbprint -replace '\s', '')) {
        throw 'APPX signature signer does not match the pinned certificate thumbprint.'
    }
    if ($signerCertificate.Thumbprint -ne $cert.Thumbprint) {
        throw 'APPX signature signer differs from the exported public certificate.'
    }

    # AppxSignature.p7x signs an SPC indirect-data digest header. Match its AXBM digest to the
    # exact block map bytes, then verify every uncompressed 64-KB payload block listed there.
    $asnReader = [System.Formats.Asn1.AsnReader]::new(
        $cms.ContentInfo.Content,
        [System.Formats.Asn1.AsnEncodingRules]::DER)
    $indirectData = $asnReader.ReadSequence()
    $spcData = $indirectData.ReadSequence()
    $null = $spcData.ReadObjectIdentifier()
    if ($spcData.HasData) { $null = $spcData.ReadEncodedValue() }
    $spcData.ThrowIfNotEmpty()
    $digestInfo = $indirectData.ReadSequence()
    $digestAlgorithm = $digestInfo.ReadSequence()
    $digestAlgorithmOid = $digestAlgorithm.ReadObjectIdentifier()
    while ($digestAlgorithm.HasData) { $null = $digestAlgorithm.ReadEncodedValue() }
    $digestAlgorithm.ThrowIfNotEmpty()
    $digestHeader = $digestInfo.ReadOctetString()
    $digestInfo.ThrowIfNotEmpty()
    $indirectData.ThrowIfNotEmpty()
    $asnReader.ThrowIfNotEmpty()
    if ($digestAlgorithmOid -ne '2.16.840.1.101.3.4.2.1' -or
        $digestHeader.Length -lt 40 -or (($digestHeader.Length - 4) % 36) -ne 0 -or
        [Text.Encoding]::ASCII.GetString($digestHeader, 0, 4) -ne 'APPX') {
        throw 'APPX signature has an invalid SHA-256 digest header.'
    }
    $signedBlockMapDigest = $null
    $digestCount = ($digestHeader.Length - 4) / 36
    for ($index = 0; $index -lt $digestCount; $index++) {
        $recordOffset = 4 + ($index * 36)
        if ([Text.Encoding]::ASCII.GetString($digestHeader, $recordOffset, 4) -eq 'AXBM') {
            if ($signedBlockMapDigest) { throw 'APPX signature contains duplicate block-map digests.' }
            $signedBlockMapDigest = [byte[]]::new(32)
            [Array]::Copy($digestHeader, $recordOffset + 4, $signedBlockMapDigest, 0, 32)
        }
    }
    if (-not $signedBlockMapDigest) { throw 'APPX signature has no block-map digest.' }
    $blockMapBytes = [IO.File]::ReadAllBytes($blockMapPath)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try { $actualBlockMapDigest = $sha256.ComputeHash($blockMapBytes) } finally { $sha256.Dispose() }
    if ([Convert]::ToHexString($actualBlockMapDigest) -ne [Convert]::ToHexString($signedBlockMapDigest)) {
        throw 'Signed APPX block-map digest does not match AppxBlockMap.xml.'
    }

    [xml]$blockMap = [Text.Encoding]::UTF8.GetString($blockMapBytes)
    $blockMapRoot = $blockMap.DocumentElement
    if ($blockMapRoot.LocalName -ne 'BlockMap' -or
        $blockMapRoot.GetAttribute('HashMethod') -notmatch '#sha256$') {
        throw 'APPX block map does not declare the expected SHA-256 format.'
    }
    $mappedFiles = @($blockMapRoot.SelectNodes("./*[local-name()='File']"))
    if ($mappedFiles.Count -eq 0) { throw 'APPX block map contains no files.' }
    $unpackRoot = [IO.Path]::GetFullPath($unpack).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $verifiedBlocks = 0
    foreach ($mappedFile in $mappedFiles) {
        $relativeName = $mappedFile.GetAttribute('Name').Replace('/', [IO.Path]::DirectorySeparatorChar)
        if ([string]::IsNullOrWhiteSpace($relativeName) -or [IO.Path]::IsPathRooted($relativeName) -or
            ($relativeName -split '[\\/]') -contains '..') {
            throw 'APPX block map contains an unsafe file path.'
        }
        $filePath = [IO.Path]::GetFullPath((Join-Path $unpackRoot $relativeName))
        if (-not $filePath.StartsWith($unpackRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'APPX block map path escaped the package directory.'
        }
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) { throw "Block-mapped file is missing: $relativeName" }
        $expectedSize = [long]::Parse($mappedFile.GetAttribute('Size'), [Globalization.CultureInfo]::InvariantCulture)
        $blocks = @($mappedFile.SelectNodes("./*[local-name()='Block']"))
        $expectedBlockCount = [int][Math]::Ceiling($expectedSize / 65536.0)
        if ($blocks.Count -ne $expectedBlockCount) { throw "Unexpected block count for $relativeName." }
        $fileStream = [IO.File]::OpenRead($filePath)
        try {
            if ($fileStream.Length -ne $expectedSize) { throw "Block-mapped size mismatch for $relativeName." }
            foreach ($block in $blocks) {
                $chunkLength = [int][Math]::Min(65536, $fileStream.Length - $fileStream.Position)
                $chunk = [byte[]]::new($chunkLength)
                $readTotal = 0
                while ($readTotal -lt $chunkLength) {
                    $read = $fileStream.Read($chunk, $readTotal, $chunkLength - $readTotal)
                    if ($read -le 0) { throw "Unexpected end of block-mapped file: $relativeName" }
                    $readTotal += $read
                }
                $sha256 = [Security.Cryptography.SHA256]::Create()
                try { $actualBlockHash = $sha256.ComputeHash($chunk) } finally { $sha256.Dispose() }
                $expectedBlockHash = [Convert]::FromBase64String($block.GetAttribute('Hash'))
                if ([Convert]::ToBase64String($actualBlockHash) -cne [Convert]::ToBase64String($expectedBlockHash)) {
                    throw "Block hash mismatch for $relativeName."
                }
                $verifiedBlocks++
            }
        } finally { $fileStream.Dispose() }
    }
    Write-Host "Verified APPX PKCS#7 signature, pinned signer, signed block map, and $verifiedBlocks payload blocks."

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
    Write-Host "Publisher: $($identity.Publisher); version: $($identity.Version)"
} finally {
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
}
