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
$issues = [System.Collections.Generic.List[object]]::new()
$expectedMachine = @{
    x86 = 0x014c
    x64 = 0x8664
    arm64 = 0xaa64
}[$Architecture]

function Add-RequiredFile {
    param(
        [string]$Category,
        [string]$RelativePath,
        [bool]$CheckArchitecture = $false
    )

    foreach ($existing in $requirements) {
        if ($existing.RelativePath -eq $RelativePath) {
            return
        }
    }

    $requirements.Add([pscustomobject]@{
        Category = $Category
        Kind = "file"
        RelativePath = $RelativePath
        ExpectedMachine = if ($CheckArchitecture) { $expectedMachine } else { $null }
    })
}

function Add-ValidationIssue {
    param(
        [string]$Category,
        [string]$RelativePath,
        [string]$Problem
    )

    $issues.Add([pscustomobject]@{
        Category = $Category
        RelativePath = $RelativePath
        Problem = $Problem
    })
}

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

function Get-PathRelativeToRoot {
    param([string]$Path)

    $rootPrefix = $rootPath.TrimEnd("\") + "\"
    if ($Path.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $Path.Substring($rootPrefix.Length)
    }
    return $Path
}

if ($PackageMode -eq "Viewer") {
    # LvtViewer.ico is embedded by the WPF build; it is not a standalone output.
    Add-RequiredFile "Viewer application" "LvtViewer.exe" $true
    Add-RequiredFile "Viewer application" "LvtViewer.dll"
    Add-RequiredFile "Viewer application" "LvtViewer.deps.json"
    Add-RequiredFile "Viewer application" "LvtViewer.runtimeconfig.json"
}

Add-RequiredFile "CLI" "lvt.exe" $true
Add-RequiredFile "XAML/WinUI TAP" "lvt_tap_$Architecture.dll" $true

Add-RequiredFile "WPF TAP" "lvt_wpf_tap_$Architecture.dll" $true
Add-RequiredFile "WPF TAP" "LvtWpfTap.dll"
Add-RequiredFile "WPF TAP" "LvtWpfTap.runtimeconfig.json"

Add-RequiredFile "WinForms TAP" "lvt_winforms_tap_$Architecture.dll" $true
Add-RequiredFile "WinForms TAP" "LvtWinFormsTap.dll"
Add-RequiredFile "WinForms TAP" "LvtWinFormsTap.runtimeconfig.json"

Add-RequiredFile "Avalonia plugin" "plugins\lvt_avalonia_plugin.dll" $true
Add-RequiredFile "Avalonia plugin" "plugins\avalonia\lvt_avalonia_tap_$Architecture.dll" $true
Add-RequiredFile "Avalonia managed publish" "plugins\avalonia\LvtAvaloniaTreeWalker.dll"
Add-RequiredFile "Avalonia managed publish" "plugins\avalonia\LvtAvaloniaTreeWalker.deps.json"
Add-RequiredFile "Avalonia managed publish" "plugins\avalonia\LvtAvaloniaTreeWalker.runtimeconfig.json"
Add-RequiredFile "Avalonia native runtime" "plugins\avalonia\av_libglesv2.dll" $true
Add-RequiredFile "Avalonia native runtime" "plugins\avalonia\libHarfBuzzSharp.dll" $true
Add-RequiredFile "Avalonia native runtime" "plugins\avalonia\libSkiaSharp.dll" $true

Add-RequiredFile "Chromium plugin" "plugins\lvt_chromium_plugin.dll" $true
Add-RequiredFile "Chromium native host" "plugins\chromium\lvt_chromium_host.exe" $true
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

$depsRelativePath = "plugins\avalonia\LvtAvaloniaTreeWalker.deps.json"
$depsPath = Join-Path $rootPath $depsRelativePath
if (Test-Path -LiteralPath $depsPath -PathType Leaf) {
    try {
        $deps = Get-Content -LiteralPath $depsPath -Raw | ConvertFrom-Json
        $ridTarget = $deps.targets.PSObject.Properties |
            Where-Object { $_.Name -like "*/win-$Architecture" } |
            Select-Object -First 1
        if ($null -eq $ridTarget) {
            Add-ValidationIssue "Avalonia dependency manifest" $depsRelativePath `
                "does not contain a win-$Architecture runtime target"
        }
        else {
            $managedDependencies = [System.Collections.Generic.HashSet[string]]::new(
                [System.StringComparer]::OrdinalIgnoreCase)
            foreach ($library in $ridTarget.Value.PSObject.Properties) {
                $runtime = $library.Value.PSObject.Properties["runtime"]
                if ($null -eq $runtime) {
                    continue
                }
                foreach ($asset in $runtime.Value.PSObject.Properties) {
                    if ($asset.Name -like "*.dll") {
                        $name = ($asset.Name -split "[/\\]")[-1]
                        [void]$managedDependencies.Add($name)
                    }
                }
            }

            $derivedDependencyCount = 0
            foreach ($name in $managedDependencies) {
                if ($name -ne "LvtAvaloniaTreeWalker.dll") {
                    Add-RequiredFile "Avalonia managed dependency" "plugins\avalonia\$name"
                    $derivedDependencyCount++
                }
            }
            if ($derivedDependencyCount -eq 0) {
                Add-ValidationIssue "Avalonia dependency manifest" $depsRelativePath `
                    "does not declare any managed runtime dependencies"
            }
        }
    }
    catch {
        Add-ValidationIssue "Avalonia dependency manifest" $depsRelativePath `
            "is not a readable dependency manifest: $($_.Exception.Message)"
    }
}

foreach ($requirement in $requirements) {
    $fullPath = Join-Path $rootPath $requirement.RelativePath
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        Add-ValidationIssue $requirement.Category $requirement.RelativePath "missing file"
        continue
    }

    if ($null -ne $requirement.ExpectedMachine) {
        $actualMachine = Get-PeMachine $fullPath
        if ($actualMachine -ne $requirement.ExpectedMachine) {
            $actual = if ($null -eq $actualMachine) {
                "not a PE file"
            }
            else {
                "PE machine 0x{0:x4}" -f $actualMachine
            }
            Add-ValidationIssue $requirement.Category $requirement.RelativePath `
                "expected $Architecture PE machine 0x$($requirement.ExpectedMachine.ToString('x4')); found $actual"
        }
    }
}

$avaloniaRuntimesPath = Join-Path $rootPath "plugins\avalonia\runtimes"
if (Test-Path -LiteralPath $avaloniaRuntimesPath -PathType Container) {
    Add-ValidationIssue "Foreign Avalonia runtime" `
        (Get-PathRelativeToRoot $avaloniaRuntimesPath) `
        "RID-specific release publishes must contain only flattened win-$Architecture native assets"
}

$avaloniaPath = Join-Path $rootPath "plugins\avalonia"
if (Test-Path -LiteralPath $avaloniaPath -PathType Container) {
    foreach ($file in Get-ChildItem -LiteralPath $avaloniaPath -File -Recurse -Force) {
        if ($file.Extension -in @(".so", ".dylib")) {
            $relativePath = Get-PathRelativeToRoot $file.FullName
            Add-ValidationIssue "Foreign Avalonia runtime" $relativePath `
                "Linux and macOS native assets must not be packaged"
        }
    }
}

if ($Layout -eq "Package" -and (Test-Path -LiteralPath $rootPath -PathType Container)) {
    $forbiddenExtensions = @(".pdb", ".ilk", ".dbg", ".dmp", ".map")
    foreach ($file in Get-ChildItem -LiteralPath $rootPath -File -Recurse -Force) {
        if ($file.Extension -in $forbiddenExtensions -or $file.Name -like "*.locked*") {
            $relativePath = Get-PathRelativeToRoot $file.FullName
            Add-ValidationIssue "Forbidden release artifact" $relativePath `
                "debug, symbol, incremental-link, dump, map, and locked files must not be packaged"
        }
    }
}

if ($issues.Count -ne 0) {
    $details = $issues | ForEach-Object {
        "  - [{0}] {1}: {2}" -f $_.Category, $_.RelativePath, $_.Problem
    }
    throw ("Release artifact validation failed for {0} {1} {2} root '{3}'. Found {4} issue(s):`n{5}" -f
        $Architecture, $PackageMode, $Layout, $rootPath, $issues.Count, ($details -join "`n"))
}

Write-Host ("Validated {0} required item(s) for {1} {2} {3}: {4}" -f
    $requirements.Count, $Architecture, $PackageMode, $Layout, $rootPath)
