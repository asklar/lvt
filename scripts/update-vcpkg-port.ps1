<#
.SYNOPSIS
    Point the in-repo vcpkg registry at a released tag.

.DESCRIPTION
    A vcpkg port has to pin the exact source archive it builds from, so the
    port's SHA512 can only be computed once the tag exists on GitHub. This
    script downloads the tarball GitHub serves for a tag, hashes it, rewrites
    ports/lvt/{vcpkg.json,portfile.cmake}, and refreshes the versions database.

.PARAMETER Version
    Release version without the leading "v", e.g. 0.2.0.

.PARAMETER Vcpkg
    Path to vcpkg.exe. Only needed to regenerate the versions database; when it
    is absent the database has to be refreshed manually afterwards.

.EXAMPLE
    ./scripts/update-vcpkg-port.ps1 -Version 0.2.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Vcpkg = $env:VCPKG_EXE
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$portDir = Join-Path $repoRoot 'ports/lvt'
$manifest = Join-Path $portDir 'vcpkg.json'
$portfile = Join-Path $portDir 'portfile.cmake'

if (-not (Test-Path $manifest)) { throw "port manifest not found: $manifest" }

$tag = "v$Version"
$url = "https://github.com/asklar/lvt/archive/$tag.tar.gz"
$tarball = Join-Path ([System.IO.Path]::GetTempPath()) "lvt-$tag.tar.gz"

Write-Host "Fetching $url"
try {
    Invoke-WebRequest -Uri $url -OutFile $tarball
} catch {
    throw "could not download $url - has $tag been pushed? ($_)"
}

$sha = (Get-FileHash $tarball -Algorithm SHA512).Hash.ToLowerInvariant()
Write-Host "SHA512 $sha"

# Version lives in the manifest; the archive hash lives in the portfile.
$json = Get-Content $manifest -Raw | ConvertFrom-Json
$json.version = $Version
$json | ConvertTo-Json -Depth 32 | Set-Content $manifest -Encoding utf8

$content = Get-Content $portfile -Raw
$updated = [regex]::Replace(
    $content,
    '(?m)^(\s*SHA512\s+)[0-9a-fA-F]+\s*$',
    { param($m) "$($m.Groups[1].Value)$sha" },
    1)
if ($updated -eq $content) { throw "no SHA512 line updated in $portfile" }
Set-Content $portfile -Value $updated -NoNewline -Encoding utf8

Write-Host "Updated ports/lvt to $Version"

# The versions database keys on the git tree hash of the port directory, so the
# port changes above have to be committed before it can be refreshed.
if ($Vcpkg -and (Test-Path $Vcpkg)) {
    & $Vcpkg x-add-version lvt --overwrite-version `
        --x-builtin-ports-root="$portDir/.." `
        --x-builtin-registry-versions-dir="$repoRoot/versions"
    if ($LASTEXITCODE -ne 0) { throw "vcpkg x-add-version failed" }
} else {
    Write-Warning "vcpkg.exe not supplied; commit the port then run:"
    Write-Warning "  vcpkg x-add-version lvt --overwrite-version --x-builtin-ports-root=ports --x-builtin-registry-versions-dir=versions"
}
