#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('plan', 'install', 'doctor', 'webui')]
    [string]$Action = 'plan'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$script:SetupPage = 'https://github.com/LizardByte/Sunshine/releases/latest'
$script:LocalWebUi = 'https://localhost:47990/'

function Get-HostSetupPlan {
    [pscustomobject]@{
        platform = 'Windows 10/11 x64 (Intel/AMD)'
        package = 'LizardByte.Sunshine'
        source = 'winget'
        installation = 'Official interactive installer; existing installations are retained'
        web_ui = $script:LocalWebUi
        configuration = 'Create credentials and pair the TV locally in Sunshine'
        personal_game_library = 'Configured by this PC owner in Sunshine Applications'
        modified_existing_settings = $false
        linux_host_patches_applied = $false
    }
}

function Assert-SupportedWindows {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'Run this helper on the Windows PC that will host your games.'
    }
    $architecture = $env:PROCESSOR_ARCHITEW6432
    if (-not $architecture) { $architecture = $env:PROCESSOR_ARCHITECTURE }
    if ($architecture -ne 'AMD64') {
        throw 'This helper supports Intel/AMD x64 Windows. See the official installer for experimental ARM64 support.'
    }
}

function Get-SunshineInstallation {
    $service = Get-Service -Name 'SunshineService' -ErrorAction SilentlyContinue
    $running = Get-Process -Name 'sunshine' -ErrorAction SilentlyContinue
    $installed = ($null -ne $service) -or ($null -ne $running)
    foreach ($key in @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )) {
        $entries = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue |
            Where-Object { $_.PSObject.Properties['DisplayName'] -and $_.DisplayName -eq 'Sunshine' }
        if ($entries) { $installed = $true }
    }
    if ($env:ProgramFiles -and (Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'Sunshine\sunshine.exe'))) {
        $installed = $true
    }
    [pscustomobject]@{
        installed = $installed
        service_state = $(if ($service) { [string]$service.Status } else { 'Not found' })
    }
}

function Get-WinGetExecutable {
    $command = Get-Command 'winget.exe' -CommandType Application -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    return $null
}

function Invoke-SunshinePackageInstall {
    param([Parameter(Mandatory = $true)][string]$Executable)
    & $Executable install --id LizardByte.Sunshine --exact --source winget --interactive --no-upgrade | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw ('Sunshine installer returned exit code {0}. See its installer output; no settings were rewritten by this helper.' -f $LASTEXITCODE)
    }
}

function Open-SetupPage {
    param([Parameter(Mandatory = $true)][string]$Url)
    Start-Process -FilePath $Url | Out-Null
}

function Test-LocalPort {
    param([int]$Port = 47990)
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $pending = $client.ConnectAsync('127.0.0.1', $Port)
        return ($pending.Wait(1500) -and $client.Connected)
    }
    catch { return $false }
    finally { $client.Dispose() }
}

function Get-HostSetupReport {
    $installation = Get-SunshineInstallation
    [pscustomobject]@{
        sunshine_installed = $installation.installed
        service_state = $installation.service_state
        local_web_ui_listening = Test-LocalPort
        web_ui = $script:LocalWebUi
        pairing = 'Approve the PIN shown by your TV in this PC Sunshine web UI'
        controller_setup = 'Choose and install a supported virtual gamepad driver in Sunshine'
        live_tv_stream_tested = $false
    }
}

function Install-WindowsHost {
    Assert-SupportedWindows
    $installation = Get-SunshineInstallation
    if (-not $installation.installed) {
        $winget = Get-WinGetExecutable
        if (-not $winget) {
            Open-SetupPage -Url $script:SetupPage
            Write-Host 'WinGet was not found. Install the official Sunshine Windows AMD64 MSI from the page, then run this helper again.'
            return [pscustomobject]@{ status = 'manual_install_required'; upgraded_existing_install = $false }
        }
        Invoke-SunshinePackageInstall -Executable $winget
        $installation = Get-SunshineInstallation
        if (-not $installation.installed) {
            throw 'Sunshine installation could not be detected. Finish or restart the official installer before pairing.'
        }
    }
    Open-SetupPage -Url $script:LocalWebUi
    Write-Host 'Use this PC Sunshine web UI to create your credentials, configure a controller driver, and pair the PIN shown on your TV.'
    Write-Host 'If the web UI does not open, start Sunshine from the Windows Start menu. Existing settings, games, pairing and service state were retained.'
    [pscustomobject]@{ status = 'ready_for_local_configuration'; upgraded_existing_install = $false }
}

function Invoke-WindowsHostSetup {
    param([string]$RequestedAction)
    switch ($RequestedAction) {
        'plan' { Get-HostSetupPlan | ConvertTo-Json -Depth 4 }
        'install' { Install-WindowsHost | ConvertTo-Json -Depth 4 }
        'doctor' { Assert-SupportedWindows; Get-HostSetupReport | ConvertTo-Json -Depth 4 }
        'webui' { Assert-SupportedWindows; Open-SetupPage -Url $script:LocalWebUi }
        default { throw 'Expected plan, install, doctor or webui.' }
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    try { Invoke-WindowsHostSetup -RequestedAction $Action }
    catch { Write-Error -Message $_.Exception.Message -ErrorAction Continue; exit 1 }
}
