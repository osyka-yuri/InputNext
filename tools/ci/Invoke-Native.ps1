#Requires -Version 7.6

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Cli,
    [Parameter(Mandatory)][string]$Toolset,
    [Parameter(Mandatory)][string]$Compiler,
    [Parameter(Mandatory)][ValidateSet('tests', 'quality', 'package', 'analyze', 'asan', 'ubsan', 'codeql-dependencies', 'codeql', 'stress')][string]$Check,
    [ValidateSet('x64', 'x86')][string]$Architecture = 'x64',
    [ValidateRange(1, 256)][int]$Jobs = [Environment]::ProcessorCount,
    [string]$Root = (Resolve-Path "$PSScriptRoot/../..").Path,
    [switch]$Plan
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
Set-StrictMode -Version Latest
$tools = Get-Content -LiteralPath $Toolset -Raw | ConvertFrom-Json -AsHashtable
$compilerInfo = Get-Content -LiteralPath $Compiler -Raw | ConvertFrom-Json
Import-Module (Join-Path $compilerInfo.installation_root 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $compilerInfo.installation_root -SkipAutomaticLocation -DevCmdArguments "-arch=$Architecture -host_arch=x64 -vcvars_ver=$($compilerInfo.vcvars_version)" | Out-Null
foreach ($entry in $tools.environment.GetEnumerator()) {
    $value = $entry.Value
    if ($entry.Key -in 'PATH', 'PSModulePath') {
        $value += [IO.Path]::PathSeparator + [Environment]::GetEnvironmentVariable($entry.Key, 'Process')
    }
    [Environment]::SetEnvironmentVariable($entry.Key, $value, 'Process')
}
$env:PATH = "$($compilerInfo.toolset_root)/bin/Hostx64/$Architecture;$env:PATH"
$arguments = @('native', '--root', $Root, '--toolset', $Toolset, '--check', $Check, '--architecture', $Architecture, '--jobs', $Jobs)
if ($Plan) {
    $arguments += '--plan'
}
& $Cli @arguments
