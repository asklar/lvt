[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [Alias("BuildRoot", "OutputRoot")]
    [string]$Root,

    [Parameter(Mandatory = $true)]
    [ValidateSet("x64", "x86", "arm64")]
    [string]$Architecture,

    [Parameter(Mandatory = $true)]
    [ValidateSet("CLI", "Viewer")]
    [string]$PackageMode,

    [ValidateSet("Build", "Package")]
    [string]$Layout = "Package"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($PackageMode -eq "Viewer" -and $Architecture -ne "x64") {
    throw "The Viewer release is x64-only; architecture '$Architecture' is not valid."
}

$rootPath = [System.IO.Path]::GetFullPath($Root)
$requirements = [System.Collections.Generic.List[object]]::new()

function Add-RequiredFile {
    param(
        [string]$Category,
        [string]$RelativePath
    )

    $requirements.Add([pscustomobject]@{
        Category = $Category
        Kind = "file"
        RelativePath = $RelativePath
    })
}

function Add-RequiredDirectory {
    param(
        [string]$Category,
        [string]$RelativePath
    )

    $requirements.Add([pscustomobject]@{
        Category = $Category
        Kind = "non-empty directory"
        RelativePath = $RelativePath
    })
}

if ($PackageMode -eq "Viewer") {
    # LvtViewer.ico is embedded by the WPF build; it is not a standalone output.
    Add-RequiredFile "Viewer application" "LvtViewer.exe"
    Add-RequiredFile "Viewer application" "LvtViewer.dll"
    Add-RequiredFile "Viewer application" "LvtViewer.deps.json"
    Add-RequiredFile "Viewer application" "LvtViewer.runtimeconfig.json"
}

Add-RequiredFile "CLI" "lvt.exe"
Add-RequiredFile "XAML/WinUI TAP" "lvt_tap_$Architecture.dll"

Add-RequiredFile "WPF TAP" "lvt_wpf_tap_$Architecture.dll"
Add-RequiredFile "WPF TAP" "LvtWpfTap.dll"
Add-RequiredFile "WPF TAP" "LvtWpfTap.runtimeconfig.json"

Add-RequiredFile "WinForms TAP" "lvt_winforms_tap_$Architecture.dll"
Add-RequiredFile "WinForms TAP" "LvtWinFormsTap.dll"
Add-RequiredFile "WinForms TAP" "LvtWinFormsTap.runtimeconfig.json"

Add-RequiredFile "Avalonia plugin" "plugins\lvt_avalonia_plugin.dll"
Add-RequiredFile "Avalonia plugin" "plugins\avalonia\lvt_avalonia_tap_$Architecture.dll"
Add-RequiredFile "Avalonia managed publish" "plugins\avalonia\LvtAvaloniaTreeWalker.dll"
Add-RequiredFile "Avalonia managed publish" "plugins\avalonia\LvtAvaloniaTreeWalker.deps.json"
Add-RequiredFile "Avalonia managed publish" "plugins\avalonia\LvtAvaloniaTreeWalker.runtimeconfig.json"
Add-RequiredDirectory "Avalonia managed publish" "plugins\avalonia\runtimes\win-$Architecture\native"

Add-RequiredFile "Chromium plugin" "plugins\lvt_chromium_plugin.dll"
Add-RequiredFile "Chromium native host" "plugins\chromium\lvt_chromium_host.exe"
Add-RequiredFile "Chromium extension" "plugins\chromium\extension\manifest.json"
Add-RequiredFile "Chromium extension" "plugins\chromium\extension\service-worker.js"
Add-RequiredFile "Chromium extension" "plugins\chromium\extension\icons\icon16.png"
Add-RequiredFile "Chromium extension" "plugins\chromium\extension\icons\icon48.png"
Add-RequiredFile "Chromium extension" "plugins\chromium\extension\icons\icon128.png"

if ($Layout -eq "Package") {
    if ($PackageMode -eq "CLI") {
        Add-RequiredFile "Packaged skill" "skills\lvt\SKILL.md"
    }
    else {
        Add-RequiredFile "Viewer documentation" "README.txt"
        Add-RequiredFile "Viewer documentation" "LICENSE"
    }
}

$missing = [System.Collections.Generic.List[object]]::new()
foreach ($requirement in $requirements) {
    $fullPath = Join-Path $rootPath $requirement.RelativePath
    $present = if ($requirement.Kind -eq "file") {
        Test-Path -LiteralPath $fullPath -PathType Leaf
    }
    else {
        (Test-Path -LiteralPath $fullPath -PathType Container) -and
            ($null -ne (Get-ChildItem -LiteralPath $fullPath -File -Recurse | Select-Object -First 1))
    }

    if (-not $present) {
        $missing.Add($requirement)
    }
}

if ($missing.Count -ne 0) {
    $details = $missing | ForEach-Object {
        "  - [{0}] {1} ({2})" -f $_.Category, $_.RelativePath, $_.Kind
    }
    throw ("Release artifact validation failed for {0} {1} {2} root '{3}'. Missing {4} required item(s):`n{5}" -f
        $Architecture, $PackageMode, $Layout, $rootPath, $missing.Count, ($details -join "`n"))
}

Write-Host ("Validated {0} required item(s) for {1} {2} {3}: {4}" -f
    $requirements.Count, $Architecture, $PackageMode, $Layout, $rootPath)
