; ScreenFleet installer (Inno Setup 6). Build with build.cmd, which builds
; the app and the driver package first. The driver ships unsigned:
; driver\install.ps1 signs it on the PC with a key it deletes at once.
;
; Installs to Program Files, so the driver scripts the app runs with admin
; rights sit in a folder only administrators can change.

#define AppExe "..\host-windows\build\ScreenFleet.exe"
#define AppVersion GetStringFileInfo(AddBackslash(SourcePath) + AppExe, "ProductVersion")
#define DriverPkg "..\driver\build\package"

[Setup]
AppId={{6F1C2B7E-4E5A-4C1B-9B8E-3D2A5F7C9E10}
AppName=ScreenFleet
AppVersion={#AppVersion}
AppVerName=ScreenFleet {#AppVersion}
AppPublisher=Harun Ethem ÇALIŞ
AppPublisherURL=https://harunethm.github.io/screenfleet/
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\ScreenFleet
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; The driver needs Windows 10 2004 or later.
MinVersion=10.0.19041
LicenseFile=..\LICENSE
SetupIconFile=..\host-windows\res\sst.ico
UninstallDisplayIcon={app}\ScreenFleet.exe
WizardStyle=modern
CloseApplications=yes
OutputDir=build
OutputBaseFilename=ScreenFleet-{#AppVersion}-setup
; HKCU below is only cleaned up, for the user running setup.
UsedUserAreasWarning=no

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"

[Files]
Source: "{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\driver\install.ps1"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "..\driver\uninstall.ps1"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "{#DriverPkg}\SstDisplay.inf"; DestDir: "{app}\driver\build\package"; Flags: ignoreversion
Source: "{#DriverPkg}\SstDisplay.dll"; DestDir: "{app}\driver\build\package"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\ScreenFleet"; Filename: "{app}\ScreenFleet.exe"
Name: "{autodesktop}\ScreenFleet"; Filename: "{app}\ScreenFleet.exe"; Tasks: desktopicon

[InstallDelete]
; Left by installs from before the rename (Second Screen).
Type: files; Name: "{app}\SecondScreen.exe"
Type: files; Name: "{autoprograms}\Second Screen.lnk"
Type: files; Name: "{autodesktop}\Second Screen.lnk"

[Registry]
; "Start with Windows" (set by the app) must not outlive it.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "ScreenFleet"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Second Screen"; Flags: deletevalue dontcreatekey

[Run]
Filename: "{app}\ScreenFleet.exe"; Description: "Start ScreenFleet"; Flags: postinstall nowait skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\driver\uninstall.ps1"" -Elevated"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveDriver"

[UninstallDelete]
; install.log, uninstall.log and the package install.ps1 signed
Type: filesandordirs; Name: "{app}\driver\build"
; PrepareToInstall makes {app} itself, so Setup doesn't know it created it.
Type: dirifempty; Name: "{app}\driver"
Type: dirifempty; Name: "{app}"

[Code]
// The app and the uninstaller run scripts from {app} with admin rights, so
// only administrators may change it, as in Program Files. Outside Program
// Files (e.g. a folder on D:) every signed-in user could otherwise swap them.
// The folder's permissions are replaced, so it must be ours: empty, new, or
// an earlier install.
function IsOurFolder(Dir: String): Boolean;
var
  Find: TFindRec;
begin
  Result := True;
  if not DirExists(Dir) or FileExists(AddBackslash(Dir) + 'ScreenFleet.exe')
     or FileExists(AddBackslash(Dir) + 'SecondScreen.exe') then Exit;
  if FindFirst(AddBackslash(Dir) + '*', Find) then begin
    try
      repeat
        if (Find.Name <> '.') and (Find.Name <> '..') then begin
          Result := False;
          Exit;
        end;
      until not FindNext(Find);
    finally
      FindClose(Find);
    end;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not IsOurFolder(WizardDirValue) then begin
    MsgBox('This folder already has other files in it. Choose an empty or new folder for ScreenFleet.', mbError, MB_OK);
    Result := False;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Dir: String;
  ResultCode: Integer;
begin
  Result := '';
  Dir := ExpandConstant('{app}');
  if not IsOurFolder(Dir) then begin
    Result := 'The folder ' + Dir + ' already has other files in it. Choose an empty or new folder.';
    Exit;
  end;
  // Administrators and SYSTEM: full control. Users: read and run.
  if not ForceDirectories(Dir) or
     not Exec(ExpandConstant('{sys}\icacls.exe'),
              '"' + Dir + '" /inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX /Q',
              '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then begin
    Result := 'Could not set the permissions of ' + Dir + '.';
    Exit;
  end;
  // What an earlier install left inside takes the folder's permissions too
  // (nothing to do, and a harmless error, when the folder is new).
  Exec(ExpandConstant('{sys}\icacls.exe'), '"' + AddBackslash(Dir) + '*" /reset /T /C /Q',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

// The driver isn't optional: without it the tablet has nothing to show.
procedure InitializeWizard;
begin
  CreateOutputMsgPage(wpLicense, 'What Setup changes on this PC',
    'ScreenFleet needs a virtual display driver; it is always installed.',
    'The driver adds a monitor to Windows that the app sends to your tablet.' + #13#10#13#10
    + 'To install it, Setup:' + #13#10
    + '  - creates a certificate for this PC only and signs the driver with it,' + #13#10
    + '  - deletes that certificate''s private key at once, so nobody (the authors included) '
    + 'can sign anything else with it,' + #13#10
    + '  - adds the certificate to the certificates Windows trusts, so Windows accepts the driver.' + #13#10#13#10
    + 'Uninstalling ScreenFleet (Settings > Apps) removes the driver and the certificate again.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then begin
    WizardForm.StatusLabel.Caption := 'Installing the virtual display driver...';
    if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
                '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\driver\install.ps1') + '" -Elevated',
                '', SW_HIDE, ewWaitUntilTerminated, ResultCode) or (ResultCode <> 0) then
      SuppressibleMsgBox('The virtual display driver could not be installed. The tablet becomes a monitor through it; '
        + 'you can install it later from the ScreenFleet window.' + #13#10#13#10
        + 'Details: ' + ExpandConstant('{app}\driver\build\install.log'), mbError, MB_OK, IDOK);
  end;
end;
