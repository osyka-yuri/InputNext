#Requires -Version 7.6
#Requires -Modules @{ ModuleName = 'PSScriptAnalyzer'; ModuleVersion = '1.25.0' }

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
$scriptFiles = Get-ChildItem -LiteralPath $PSScriptRoot -Recurse -File -Include '*.ps1', '*.psm1', '*.psd1'

foreach ($file in $scriptFiles) {
    $source = [IO.File]::ReadAllText($file.FullName).Replace("`r`n", "`n")
    $formatted = (Invoke-Formatter -ScriptDefinition $source -Settings CodeFormattingOTBS).Replace("`r`n", "`n")
    if ($source -cne $formatted) {
        throw "PowerShell formatting differs: $($file.FullName). Run Invoke-Formatter with CodeFormattingOTBS."
    }
    $diagnostics = @(Invoke-ScriptAnalyzer -Path $file.FullName)
    if ($diagnostics.Count -gt 0) {
        $diagnostics | Format-Table -Property ScriptName, Line, RuleName, Message -AutoSize | Out-String | Write-Information -InformationAction Continue
        throw "PowerShell analysis failed: $($file.FullName)"
    }
}

$formatter = (Get-Command -Name 'clang-format' -CommandType Application -ErrorAction Stop).Source
$version = & $formatter --version
if ($version -notmatch '^clang-format version 23\.1\.3(\s|$)') {
    throw 'InputNext requires clang-format 23.1.3.'
}

$cppFiles = foreach ($root in 'src', 'include', 'sdk', 'tests', 'tools', 'examples') {
    $path = Join-Path $projectRoot $root
    if (Test-Path -LiteralPath $path) {
        Get-ChildItem -LiteralPath $path -Recurse -File -Include '*.cpp', '*.hpp', '*.h', '*.c'
    }
}
if ($cppFiles) {
    $fileListPath = [IO.Path]::GetTempFileName()
    try {
        [string[]]$paths = $cppFiles.FullName
        [IO.File]::WriteAllLines($fileListPath, $paths, [Text.UTF8Encoding]::new($false))
        & $formatter --dry-run --Werror --fail-on-incomplete-format "--files=$fileListPath"
    } finally {
        Remove-Item -LiteralPath $fileListPath -Force
    }
}

Write-Information 'PowerShell analysis, PowerShell formatting, and C/C++ formatting passed.' -InformationAction Continue
