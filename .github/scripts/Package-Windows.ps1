[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group

    Log-Group "Building the ${ProductName} installer..."
    $Iscc = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ( ! $Iscc ) {
        choco install innosetup --yes --no-progress | Out-Null
        $Iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
    }
    $InstallerName = "${OutputName}-installer"
    $IsccArgs = @(
        "/DAppVersion=${ProductVersion}",
        "/DSourceDir=${ProjectRoot}\release\${Configuration}",
        "/DOutputDir=${ProjectRoot}\release",
        "/DOutputName=${InstallerName}",
        "${ProjectRoot}\build-aux\installer\chiripy.iss"
    )
    & $Iscc @IsccArgs
    if ( $LASTEXITCODE -ne 0 ) { throw "ISCC failed with exit code ${LASTEXITCODE}" }
    Log-Group

    Log-Group "Smoke-testing the installer: install, reinstall over a stale file, uninstall..."
    Test-Installer -Installer "${ProjectRoot}\release\${InstallerName}.exe"
    Log-Group
}

# Proves the installer is idempotent on every CI build: a second run over an
# existing install must succeed, clear files an older version left behind,
# and leave exactly one uninstall entry; uninstall must remove the plugin.
function Test-Installer {
    param([string] $Installer)

    $PluginDir = "${env:ProgramData}\obs-studio\plugins\chiripy"
    $Dll = "${PluginDir}\bin\64bit\chiripy.dll"
    $Overlay = "${PluginDir}\data\overlay.html"
    $Stale = "${PluginDir}\bin\64bit\left-over-from-an-older-version.txt"
    $Silent = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')

    function Invoke-Setup([string] $Exe, [string] $Log) {
        $p = Start-Process -FilePath $Exe -ArgumentList ($Silent + "/LOG=${Log}") -Wait -PassThru
        if ( $p.ExitCode -ne 0 ) {
            Get-Content $Log -ErrorAction SilentlyContinue | Select-Object -Last 30
            throw "${Exe} exited with $($p.ExitCode)"
        }
    }
    function Get-UninstallEntries {
        Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall' |
            Where-Object { (Get-ItemProperty $_.PSPath).DisplayName -like 'Chiripy Chat*' }
    }

    Invoke-Setup $Installer "${env:RUNNER_TEMP}\chiripy-install-1.log"
    if ( ! ( (Test-Path $Dll) -and (Test-Path $Overlay) ) ) { throw 'First install did not place the plugin files' }

    Set-Content -Path $Stale -Value 'stale'
    Invoke-Setup $Installer "${env:RUNNER_TEMP}\chiripy-install-2.log"
    if ( Test-Path $Stale ) { throw 'Reinstall left a stale file behind' }
    if ( ! (Test-Path $Dll) ) { throw 'Reinstall lost the plugin DLL' }
    $Entries = @(Get-UninstallEntries)
    if ( $Entries.Count -ne 1 ) { throw "Expected one uninstall entry, found $($Entries.Count)" }

    # The Inno uninstaller relaunches itself from %TEMP% and returns at once,
    # so wait for the files to disappear rather than for the process.
    Start-Process -FilePath "${PluginDir}\unins000.exe" -ArgumentList $Silent -Wait
    for ( $i = 0; $i -lt 60 -and (Test-Path $Dll); $i++ ) { Start-Sleep -Milliseconds 500 }
    if ( Test-Path $Dll ) { throw 'Uninstall did not remove the plugin DLL' }
    if ( @(Get-UninstallEntries).Count -ne 0 ) { throw 'Uninstall left its Apps & features entry' }

    Write-Output 'Installer smoke test passed: install, idempotent reinstall, uninstall.'
}

Package
