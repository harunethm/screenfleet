# Installs (or updates) the ScreenFleet virtual display driver.
# Asks for admin rights (UAC) and logs to build\install.log.
#
# The package (build\package: SstDisplay.inf + .dll) ships unsigned and is
# signed here, on this PC, with a certificate made for this one install:
#  1. Creates a certificate, adds its public part to Local Machine "Trusted
#     Root" and "Trusted Publishers", signs a copy of the package
#     (build\signed) and deletes the private key at once. Nobody, us
#     included, holds a key this PC trusts.
#  2. Adds the signed package to the Windows driver store (pnputil).
#  3. Creates the virtual adapter device Root\SstDisplay (what devcon does,
#     done here so the WDK isn't needed on the PC).
#  4. Removes certificates and driver packages of earlier installs.
# uninstall.ps1 reverses it all.
param([switch]$Elevated)

$ErrorActionPreference = 'Stop'
$pkg = Join-Path $PSScriptRoot 'build\package'
$signed = Join-Path $PSScriptRoot 'build\signed'
$log = Join-Path $PSScriptRoot 'build\install.log'
$hwid = 'Root\SstDisplay'
$subject = 'CN=ScreenFleet Driver (signed on this PC)'
# Certificates of earlier installs, also from before the rename (Second
# Screen) and the old shared developer one.
$subjects = $subject, 'CN=Second Screen Driver (signed on this PC)', 'CN=Second Screen Local Driver Signing'

# devcon's "install" and "update", without devcon: register a root-enumerated
# device with our hardware ID, then point it at our INF.
Add-Type @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class SstDevice {
    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid classGuid, IntPtr hwnd);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name, ref Guid classGuid, string desc, IntPtr hwnd, int flags, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA data, int property, byte[] buffer, int size);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiCallClassInstaller(int function, IntPtr set, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll")]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hardwareId, string infPath, int flags, out bool reboot);

    const int DICD_GENERATE_ID = 1, SPDRP_HARDWAREID = 1, DIF_REMOVE = 5, DIF_REGISTERDEVICE = 0x19, INSTALLFLAG_FORCE = 1;
    static readonly Guid DisplayClass = new Guid("4d36e968-e325-11ce-bfc1-08002be10318");

    // Returns true when Windows needs a restart to finish.
    public static bool Install(string hardwareId, string infPath, bool create) {
        if (!create) return Update(hardwareId, infPath);
        Guid cls = DisplayClass;
        IntPtr set = SetupDiCreateDeviceInfoList(ref cls, IntPtr.Zero);
        if (set == new IntPtr(-1)) throw new Win32Exception();
        try {
            var data = new SP_DEVINFO_DATA { cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA)) };
            if (!SetupDiCreateDeviceInfoW(set, "Display", ref cls, null, IntPtr.Zero, DICD_GENERATE_ID, ref data))
                throw new Win32Exception();
            byte[] ids = System.Text.Encoding.Unicode.GetBytes(hardwareId + "\0\0");
            if (!SetupDiSetDeviceRegistryPropertyW(set, ref data, SPDRP_HARDWAREID, ids, ids.Length) ||
                !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref data))
                throw new Win32Exception();
            try {
                return Update(hardwareId, infPath);
            } catch {
                SetupDiCallClassInstaller(DIF_REMOVE, set, ref data);
                throw;
            }
        } finally {
            SetupDiDestroyDeviceInfoList(set);
        }
    }

    static bool Update(string hardwareId, string infPath) {
        bool reboot;
        if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, hardwareId, infPath, INSTALLFLAG_FORCE, out reboot))
            throw new Win32Exception();
        return reboot;
    }
}
'@

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    if ($Elevated) { throw "Could not get admin rights." }
    $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Elevated')
    if (Test-Path $log) { Get-Content $log }
    exit $p.ExitCode
}

Start-Transcript -Path $log -Force | Out-Null
try {
    if (-not (Test-Path "$pkg\SstDisplay.inf")) { throw "Run build.cmd first." }

    if (Test-Path $signed) { Remove-Item -Recurse -Force $signed }
    New-Item -ItemType Directory $signed | Out-Null
    Copy-Item "$pkg\SstDisplay.inf", "$pkg\SstDisplay.dll" $signed

    # No timestamp, so the signature only holds while the certificate is
    # valid: hence the long lifetime.
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
        -CertStoreLocation Cert:\LocalMachine\My -KeyExportPolicy NonExportable `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(50)
    try {
        $cer = Join-Path $signed 'SstDisplay.cer'
        Export-Certificate -Cert $cert -FilePath $cer | Out-Null
        foreach ($store in 'Root', 'TrustedPublisher') {
            Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
        }
        Remove-Item $cer
        Write-Host "Certificate $($cert.Thumbprint) trusted"

        $dll = Set-AuthenticodeSignature "$signed\SstDisplay.dll" -Certificate $cert -HashAlgorithm SHA256
        if ($dll.Status -ne 'Valid') { throw "signing SstDisplay.dll failed: $($dll.StatusMessage)" }
        # The catalog holds the hashes of the INF and the DLL (what Inf2Cat
        # makes, without needing the WDK).
        New-FileCatalog -Path $signed -CatalogFilePath "$signed\SstDisplay.cat" -CatalogVersion 2.0 | Out-Null
        $cat = Set-AuthenticodeSignature "$signed\SstDisplay.cat" -Certificate $cert -HashAlgorithm SHA256
        if ($cat.Status -ne 'Valid') { throw "signing SstDisplay.cat failed: $($cat.StatusMessage)" }
        Write-Host 'Package signed'
    } finally {
        Remove-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey
        Write-Host 'Signing key deleted'
    }

    # Windows keeps its own copy of a version it already has, signed by an
    # earlier certificate; remove it so this signature is the one in use.
    $version = ((Get-Content "$signed\SstDisplay.inf") -match '^\s*DriverVer\s*=' | Select-Object -First 1).Split(',')[1].Trim()
    foreach ($drv in Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\sstdisplay.inf' -and $_.Version -eq $version }) {
        pnputil.exe /delete-driver $drv.Driver /uninstall /force | Out-Null
        Write-Host "Replacing $($drv.Driver) (same version $version)"
    }

    pnputil.exe /add-driver "$signed\SstDisplay.inf" /install
    # 3010 = restart needed, 259 = this version is already installed.
    if ($LASTEXITCODE -and $LASTEXITCODE -ne 3010 -and $LASTEXITCODE -ne 259) { throw "pnputil failed ($LASTEXITCODE)" }

    $existing = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.HardwareID -contains $hwid }
    $inf = (Resolve-Path "$signed\SstDisplay.inf").Path
    if ([SstDevice]::Install($hwid, $inf, -not $existing)) { Write-Host 'Restart Windows to finish.' }

    $device = Get-PnpDevice -PresentOnly | Where-Object { $_.HardwareID -contains $hwid }
    $device | Format-Table -AutoSize Status, Class, FriendlyName, InstanceId | Out-String | Write-Host

    # Only once the device runs the package just signed: drop what earlier
    # installs left.
    $inUse = ($device | Get-PnpDeviceProperty DEVPKEY_Device_DriverInfPath).Data
    $ours = @(Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\sstdisplay.inf' })
    $running = $ours | Where-Object { $_.Driver -eq $inUse }
    if (-not $running) { throw "the device isn't running our driver" }
    $signer = (Get-AuthenticodeSignature (Join-Path (Split-Path $running.OriginalFileName) 'sstdisplay.cat')).SignerCertificate.Thumbprint
    if ($signer -ne $cert.Thumbprint) { throw "the device runs a package signed by $signer, not this install's $($cert.Thumbprint)" }
    foreach ($drv in $ours | Where-Object { $_.Driver -ne $inUse }) {
        pnputil.exe /delete-driver $drv.Driver | Out-Null
        Write-Host "Removed old driver package $($drv.Driver)"
    }
    # CA: Windows caches the signer of every package it checks there.
    foreach ($store in 'Root', 'TrustedPublisher', 'CA') {
        Get-ChildItem "Cert:\LocalMachine\$store" |
            Where-Object { $subjects -contains $_.Subject -and $_.Thumbprint -ne $signer } |
            ForEach-Object { Remove-Item $_.PSPath; Write-Host "Removed unused certificate $($_.Thumbprint) ($store)" }
    }
    # The driver store has its own copy now.
    Remove-Item -Recurse -Force $signed
    Write-Host "Driver signed by $signer"
    Write-Host 'Done.'
    exit 0
} catch {
    Write-Host "ERROR: $_"
    exit 1
} finally {
    Stop-Transcript | Out-Null
}
