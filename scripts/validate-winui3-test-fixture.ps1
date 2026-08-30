[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Root,

    [Parameter(Mandatory = $true)]
    [string]$IntermediateRoot,

    [Parameter(Mandatory = $true)]
    [ValidateSet("x64", "x86", "arm64")]
    [string]$Architecture
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$rootPath = (Resolve-Path -LiteralPath $Root).ProviderPath
$intermediatePath = (Resolve-Path -LiteralPath $IntermediateRoot).ProviderPath
if ($rootPath -eq $intermediatePath) {
    throw "WinUI fixture publish and intermediate directories must be distinct."
}

$expectedMachine = @{
    x86 = 0x014c
    x64 = 0x8664
    arm64 = 0xaa64
}[$Architecture]

function Get-PeMachine {
    param([string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    $reader = [System.IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) {
            return $null
        }
        $stream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0 -or $peOffset + 6 -gt $stream.Length) {
            return $null
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            return $null
        }
        return [int]$reader.ReadUInt16()
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

foreach ($relativePath in @(
    "WinUI3Sample.exe",
    "WinUI3Sample.deps.json",
    "WinUI3Sample.runtimeconfig.json",
    "App.xbf",
    "MainWindow.xbf"
)) {
    $path = Join-Path $rootPath $relativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "WinUI fixture is missing '$path'."
    }
}

foreach ($xbf in @("App.xbf", "MainWindow.xbf")) {
    $path = Join-Path $intermediatePath $xbf
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "WinUI fixture intermediate output is missing '$path'."
    }
}

$exePath = Join-Path $rootPath "WinUI3Sample.exe"
$actualMachine = Get-PeMachine $exePath
if ($actualMachine -ne $expectedMachine) {
    $actual = if ($null -eq $actualMachine) {
        "not a PE file"
    }
    else {
        "PE machine 0x{0:x4}" -f $actualMachine
    }
    throw "Expected $Architecture fixture machine 0x$($expectedMachine.ToString('x4')); found $actual."
}

$depsPath = Join-Path $rootPath "WinUI3Sample.deps.json"
$deps = Get-Content -LiteralPath $depsPath -Raw | ConvertFrom-Json
$expectedRid = "win-$Architecture"
if ($deps.runtimeTarget.name -notlike "*/$expectedRid") {
    throw "Fixture dependency manifest targets '$($deps.runtimeTarget.name)', expected '$expectedRid'."
}

Write-Host "Validated $Architecture WinUI fixture at '$rootPath'."
