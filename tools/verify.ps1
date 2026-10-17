#Requires -Version 7.6

[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86')][string]$Architecture = 'x64',
    [ValidateSet('Default', 'Analyze', 'Address', 'Undefined')][string]$NativeProfile = 'Default',
    [string]$TestRegex,
    [switch]$CheckQuality,
    [switch]$IncludeStress
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Architecture = $Architecture.ToLowerInvariant()

if ($NativeProfile -eq 'Undefined' -and $Architecture -ne 'x64') {
    throw 'The Undefined native profile is supported only for x64.'
}
if ($NativeProfile -eq 'Analyze' -and $Architecture -ne 'x64') {
    throw 'The Analyze native profile is supported only for x64.'
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$basePreset = "windows-$Architecture"
$preset = switch ($NativeProfile) {
    Analyze {
        'windows-x64-analyze'
    }
    Address {
        "windows-$Architecture-asan"
    }
    Undefined {
        'windows-x64-ubsan'
    }
    default {
        $basePreset
    }
}
$nativePresets = @($preset)
if ($NativeProfile -eq 'Analyze') {
    $nativePresets += "$preset-production"
}
$runId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$resultRoot = Join-Path $projectRoot "build/$preset/verification/$runId"
$null = New-Item -ItemType Directory -Path $resultRoot -Force

function Invoke-VerificationStep([string]$Program, [string[]]$Arguments, [string]$LogName) {
    $PSNativeCommandUseErrorActionPreference = $false
    Write-Information "$Program $($Arguments -join ' ')" -InformationAction Continue
    & $Program @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $resultRoot $LogName)
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE. Results: $resultRoot"
    }
}

Push-Location $projectRoot
try {
    if ($CheckQuality) {
        & (Join-Path $PSScriptRoot 'check_quality.ps1') *>&1 | Tee-Object -FilePath (Join-Path $resultRoot 'quality.log')
    }
    $tidy = ($CheckQuality -and $NativeProfile -eq 'Default') ? 'ON' : 'OFF'
    $stress = $IncludeStress ? 'ON' : 'OFF'
    $nativePath = $env:PATH
    try {
        foreach ($nativePreset in $nativePresets) {
            $configureArguments = @(
                '--preset', $nativePreset,
                "-DINPUTNEXT_ENABLE_CLANG_TIDY=$tidy",
                "-DINPUTNEXT_BUILD_STRESS_TESTS=$stress"
            )
            $configureLog = "configure-$($nativePreset.Substring('windows-'.Length)).log"
            Invoke-VerificationStep -Program cmake -Arguments $configureArguments -LogName $configureLog

            if ($NativeProfile -eq 'Address') {
                $runtimePathFile = Join-Path $projectRoot "build/$nativePreset/asan-runtime-path.txt"
                if (-not (Test-Path -LiteralPath $runtimePathFile -PathType Leaf)) {
                    throw "CMake did not create the expected AddressSanitizer runtime path file: $runtimePathFile"
                }

                $addressRuntime = (Get-Content -LiteralPath $runtimePathFile -Raw).Trim()
                if (-not [System.IO.Path]::IsPathFullyQualified($addressRuntime)) {
                    throw "CMake emitted a non-absolute AddressSanitizer runtime path: '$addressRuntime'"
                }
                if (-not (Test-Path -LiteralPath $addressRuntime -PathType Leaf)) {
                    throw "MSVC AddressSanitizer runtime emitted by CMake does not exist: $addressRuntime"
                }
                $runtimeDirectory = Split-Path -Parent $addressRuntime
                $env:PATH = "$runtimeDirectory;$nativePath"
            }

            $buildLog = "build-$($nativePreset.Substring('windows-'.Length)).log"
            $buildArguments = @('--build', '--preset', $nativePreset)
            Invoke-VerificationStep -Program cmake -Arguments $buildArguments -LogName $buildLog
        }
        $testArguments = @('--preset', $preset, '--output-junit', (Join-Path $resultRoot 'ctest.xml'))
        if ($TestRegex) {
            $testArguments += @('-R', $TestRegex)
        }
        Invoke-VerificationStep -Program ctest -Arguments $testArguments -LogName 'test.log'
    } finally {
        $env:PATH = $nativePath
    }
    Write-Information "Results: $resultRoot" -InformationAction Continue
} finally {
    Pop-Location
}
