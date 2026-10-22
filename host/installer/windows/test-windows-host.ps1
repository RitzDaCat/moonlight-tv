#requires -Version 5.1
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-host.ps1')

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$script:TestsPassed = 0
function Test-Case {
    param([string]$Name, [scriptblock]$Body)
    & $Body
    $script:TestsPassed++
    Write-Host ('PASS ' + $Name)
}

$original = @{}
foreach ($name in @('Get-SunshineInstallation', 'Get-WinGetExecutable', 'Invoke-SunshinePackageInstall', 'Open-SetupPage', 'Test-LocalPort')) {
    $original[$name] = (Get-Command $name -CommandType Function).ScriptBlock
}

$script:Installed = $false
$script:InstallerCalls = 0
$script:Pages = New-Object 'System.Collections.Generic.List[string]'
function Get-SunshineInstallation {
    [pscustomobject]@{ installed = $script:Installed; service_state = 'Stopped' }
}
function Get-WinGetExecutable { 'test-winget.exe' }
function Invoke-SunshinePackageInstall {
    param([string]$Executable)
    Assert-True ($Executable -eq 'test-winget.exe') 'Unexpected package manager executable'
    $script:InstallerCalls++
    $script:Installed = $true
}
function Open-SetupPage { param([string]$Url) $script:Pages.Add($Url) }

Test-Case 'plan is read-only and does not contain a personal host' {
    $plan = Get-HostSetupPlan
    Assert-True ($plan.package -eq 'LizardByte.Sunshine') 'Wrong package'
    Assert-True (-not $plan.modified_existing_settings) 'Plan mutates settings'
    Assert-True (-not $plan.linux_host_patches_applied) 'Linux patches must remain source-only'
    Assert-True ($script:InstallerCalls -eq 0 -and $script:Pages.Count -eq 0) 'Plan performed an action'
}

Test-Case 'fresh install uses the package manager and opens only the local UI' {
    $result = Install-WindowsHost
    Assert-True ($result.status -eq 'ready_for_local_configuration') 'Fresh setup was not detected'
    Assert-True ($script:InstallerCalls -eq 1) 'Installer did not run exactly once'
    Assert-True ($script:Pages.Count -eq 1 -and $script:Pages[0] -eq 'https://localhost:47990/') 'Unexpected setup URL'
}

Test-Case 'repeat install retains an existing host without upgrading or restarting it' {
    $script:Pages.Clear()
    $result = Install-WindowsHost
    Assert-True ($script:InstallerCalls -eq 1) 'Repeated setup reran the installer'
    Assert-True (-not $result.upgraded_existing_install) 'Existing host was upgraded'
    Assert-True ($script:Pages.Count -eq 1) 'Existing setup did not offer its local UI'
}

Test-Case 'missing WinGet opens official manual installation without claiming success' {
    $script:Installed = $false
    $script:Pages.Clear()
    function Get-WinGetExecutable { return $null }
    $result = Install-WindowsHost
    Assert-True ($result.status -eq 'manual_install_required') 'Missing WinGet falsely reported success'
    Assert-True ($script:InstallerCalls -eq 1) 'Missing WinGet ran an installer'
    Assert-True ($script:Pages[0] -eq 'https://github.com/LizardByte/Sunshine/releases/latest') 'Unexpected download origin'
}

Test-Case 'failed installation does not open pairing or claim configuration success' {
    $script:Pages.Clear()
    function Invoke-SunshinePackageInstall { param([string]$Executable) throw 'installer refused' }
    $caught = $false
    try { Install-WindowsHost | Out-Null } catch { $caught = $_.Exception.Message -eq 'installer refused' }
    Assert-True $caught 'Installer failure did not propagate'
    Assert-True ($script:Pages.Count -eq 0) 'Failed install opened pairing'
}

Test-Case 'missing installation after a package-manager success is detected' {
    function Invoke-SunshinePackageInstall { param([string]$Executable) }
    $caught = $false
    try { Install-WindowsHost | Out-Null } catch { $caught = $_.Exception.Message -like '*could not be detected*' }
    Assert-True $caught 'Undetected installation falsely reported success'
}

Test-Case 'doctor reports readiness without credentials or pairing state' {
    function Test-LocalPort { $true }
    $report = Get-HostSetupReport
    Assert-True $report.local_web_ui_listening 'Listening port was not reflected'
    Assert-True (-not $report.live_tv_stream_tested) 'Doctor claimed a TV streaming test'
    $keys = @($report.PSObject.Properties.Name)
    Assert-True (-not ($keys -contains 'username') -and -not ($keys -contains 'password')) 'Doctor exposed credentials'
}

Test-Case 'native WinGet invocation is exact, interactive and does not force upgrades' {
    $temporary = Join-Path ([IO.Path]::GetTempPath()) ('moonlight-windows-test-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporary | Out-Null
    try {
        $log = Join-Path $temporary 'arguments.txt'
        $stub = Join-Path $temporary 'winget-stub.cmd'
        [IO.File]::WriteAllText($stub, ('@echo off' + "`r`n" + 'echo %*>"' + $log + '"' + "`r`n" + 'exit /b 0' + "`r`n"))
        & $original['Invoke-SunshinePackageInstall'] -Executable $stub
        $arguments = [IO.File]::ReadAllText($log).Trim()
        Assert-True ($arguments -eq 'install --id LizardByte.Sunshine --exact --source winget --interactive --no-upgrade') 'Unexpected installer arguments'
        [IO.File]::WriteAllText($stub, '@exit /b 7' + "`r`n")
        $caught = $false
        try { & $original['Invoke-SunshinePackageInstall'] -Executable $stub } catch { $caught = $_.Exception.Message -like '*exit code 7*' }
        Assert-True $caught 'Native installer exit code was ignored'
    }
    finally { Remove-Item -LiteralPath $temporary -Recurse -Force }
}

Test-Case 'loopback health probe observes a listener and releases its socket' {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    try {
        $listener.Start()
        $port = $listener.LocalEndpoint.Port
        Assert-True (& $original['Test-LocalPort'] -Port $port) 'Listening loopback port was missed'
    }
    finally { $listener.Stop() }
    Assert-True (-not (& $original['Test-LocalPort'] -Port $port)) 'Closed port was reported as listening'
}

Write-Host ('Windows helper tests passed: ' + $script:TestsPassed)
