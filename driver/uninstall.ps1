# Removes the ScreenFleet virtual display driver and everything install.ps1
# added: the device, the driver packages and the trusted certificates.
# Asks for admin rights (UAC) and logs to build\uninstall.log.
param([switch]$Elevated)

$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot 'build\uninstall.log'
$hwid = 'Root\SstDisplay'
# Also the names from before the rename (Second Screen).
$subjects = 'CN=ScreenFleet Driver (signed on this PC)', 'CN=Second Screen Driver (signed on this PC)', 'CN=Second Screen Local Driver Signing'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    if ($Elevated) { throw "Could not get admin rights." }
    New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Elevated')
    if (Test-Path $log) { Get-Content $log }
    exit $p.ExitCode
}

Start-Transcript -Path $log -Force | Out-Null
try {
    foreach ($dev in Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $hwid }) {
        pnputil.exe /remove-device $dev.InstanceId
    }

    # Driver packages are renamed to oemNN.inf in the store; find ours.
    foreach ($drv in Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\sstdisplay.inf' }) {
        pnputil.exe /delete-driver $drv.Driver /uninstall /force
    }

    # CA: Windows caches the signer of every package it checks there.
    foreach ($store in 'Root', 'TrustedPublisher', 'CA') {
        Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $subjects -contains $_.Subject } | Remove-Item
        Write-Host "Certificate removed ($store)"
    }
    Write-Host 'Done.'
} catch {
    Write-Host "ERROR: $_"
    exit 1
} finally {
    Stop-Transcript | Out-Null
}
