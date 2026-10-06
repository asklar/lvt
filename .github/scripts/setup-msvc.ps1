[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("x64", "x86", "arm64", "amd64_x86", "amd64_arm64")]
    [string]$Architecture
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$targetArchitecture = switch ($Architecture) {
    "amd64_x86" { "x86" }
    "amd64_arm64" { "arm64" }
    default { $Architecture }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe was not found at '$vswhere'."
}

$installationPath = & $vswhere `
    -latest `
    -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installationPath)) {
    throw "A Visual Studio installation with the C++ toolchain was not found."
}

$devCmd = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path -LiteralPath $devCmd -PathType Leaf)) {
    throw "VsDevCmd.bat was not found at '$devCmd'."
}

$vswhereDirectory = Split-Path -Parent $vswhere
$environment = & $env:ComSpec /s /c `
    "set `"PATH=$vswhereDirectory;%PATH%`" && call `"$devCmd`" -no_logo -arch=$targetArchitecture -host_arch=x64 && set"
if ($LASTEXITCODE -ne 0) {
    throw "VsDevCmd.bat failed for target architecture '$targetArchitecture'."
}

$githubEnvironment = $env:GITHUB_ENV
if ([string]::IsNullOrWhiteSpace($githubEnvironment)) {
    throw "GITHUB_ENV is not set; this script must run in GitHub Actions."
}

$writer = [System.IO.StreamWriter]::new(
    $githubEnvironment,
    $true,
    [System.Text.UTF8Encoding]::new($false))
try {
    foreach ($line in $environment) {
        if ($line -match "^([^=][^=]*)=(.*)$") {
            $name = $Matches[1]
            $value = $Matches[2]
            $writer.WriteLine($name + "=" + $value)
        }
    }
}
finally {
    $writer.Dispose()
}

Write-Host "Configured MSVC for $targetArchitecture using '$installationPath'."
