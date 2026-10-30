#Requires -Version 7.6

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][ValidateSet('x64', 'x86')][string]$Architecture,
    [switch]$Symbols,
    [switch]$RunTrustedChecks
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Architecture = $Architecture.ToLowerInvariant()
$projectRoot = Split-Path -Parent $PSScriptRoot
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$buildPath = (Resolve-Path -LiteralPath $BuildDirectory).Path
$run = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N')
$resultRoot = Join-Path $buildPath "package-verification/$run"
$extracted = Join-Path $resultRoot 'package'
$workingDirectory = Join-Path $resultRoot 'working'
$null = New-Item -ItemType Directory -Path $workingDirectory -Force

function Invoke-PackageCheck([string]$Program, [string[]]$Arguments, [string]$Name, [switch]$DeveloperEnvironment) {
    $start = [Diagnostics.ProcessStartInfo]::new($Program)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WorkingDirectory = $workingDirectory
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    if (-not $DeveloperEnvironment) {
        $start.Environment['PATH'] = "$env:SystemRoot/system32;$env:SystemRoot"
    }
    foreach ($argument in $Arguments) {
        $start.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit(60000)
        if ($timedOut) {
            $process.Kill($true)
            $process.WaitForExit()
        }
        $text = $output.GetAwaiter().GetResult() + $errors.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $resultRoot "$Name.log"), $text)
        if ($timedOut -or $process.ExitCode -ne 0) {
            throw "$Name failed; timeout=$timedOut; exit=$($process.ExitCode). Results: $resultRoot"
        }
    } finally {
        $process.Dispose()
    }
}

$archive = [IO.Compression.ZipFile]::OpenRead($packagePath)
try {
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $archive.Entries) {
        $name = $entry.FullName
        if ($name -match '(^/|(^|/)\.\.?(/|$)|[\\:])' -or -not $names.Add($name)) {
            throw "Invalid or duplicate ZIP entry: $name"
        }
    }
} finally {
    $archive.Dispose()
}
[IO.Compression.ZipFile]::ExtractToDirectory($packagePath, $extracted)
$cmake = (Get-Command cmake -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$manifestArguments = @(
    "-DPACKAGE_ROOT=$extracted",
    "-DPACKAGE_ARCHITECTURE=$Architecture",
    "-DPACKAGE_SYMBOLS=$($Symbols.IsPresent)",
    '-P', (Join-Path $projectRoot 'cmake/VerifyPackageManifest.cmake')
)
Invoke-PackageCheck -Program $cmake -Arguments $manifestArguments -Name 'manifest'
if ($RunTrustedChecks -and -not $Symbols) {
    $checks = Get-Content -LiteralPath (Join-Path $buildPath 'distribution/checks.json') -Raw | ConvertFrom-Json
    $bin = Join-Path $extracted 'bin'
    Invoke-PackageCheck -Program $checks.probe -Arguments @($bin) -Name 'exports'
    $runtime = Join-Path $bin 'inputnext_runtime.dll'
    $consumerBuild = Join-Path $resultRoot 'sdk-build'
    $configureArguments = @(
        '-S', (Join-Path $extracted 'examples'), '-B', $consumerBuild, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_C_COMPILER=$($checks.compiler)",
        "-DCMAKE_CXX_COMPILER=$($checks.compiler)", "-DCMAKE_MAKE_PROGRAM=$($checks.ninja)"
    )
    Invoke-PackageCheck -Program $cmake -Arguments $configureArguments -Name 'sdk-configure' -DeveloperEnvironment
    Invoke-PackageCheck -Program $cmake -Arguments @('--build', $consumerBuild, '--parallel') -Name 'sdk-build' -DeveloperEnvironment
    Invoke-PackageCheck -Program (Join-Path $consumerBuild 'inputnext_core_example.exe') -Arguments @($runtime) -Name 'core-sdk'
    Invoke-PackageCheck -Program (Join-Path $consumerBuild 'inputnext_effects_example.exe') -Arguments @($runtime) -Name 'effects-sdk'
    $importDirectory = Join-Path $resultRoot 'import'
    $null = New-Item -ItemType Directory -Path $importDirectory
    Copy-Item -Path (Join-Path $bin '*') -Destination $importDirectory
    $importProbe = Join-Path $importDirectory 'inputnext_xinput_import_probe.exe'
    Copy-Item -LiteralPath $checks.import -Destination $importProbe
    Invoke-PackageCheck -Program $importProbe -Arguments @() -Name 'normal-import'
    Invoke-PackageCheck -Program $cmake -Arguments $manifestArguments -Name 'manifest-after-checks'
}
$scope = ($RunTrustedChecks -and -not $Symbols) ? 'Contents, hashes, and trusted integration checks' : 'Contents and hashes'
Write-Information "$scope verified: $packagePath. Results: $resultRoot" -InformationAction Continue
