; Chiripy Chat -- Windows installer (Inno Setup 6)
; Built by .github/scripts/Package-Windows.ps1, which passes AppVersion,
; SourceDir (the release/<config> tree), OutputDir and OutputName.
;
; Idempotent by design: AppId below is fixed forever, so running any version
; again upgrades in place with a single "Apps & features" entry; InstallDelete
; clears the previous version's files before copying, so nothing stale
; survives an upgrade. Never change AppId.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\release\RelWithDebInfo"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\release"
#endif
#ifndef OutputName
  #define OutputName "chiripy-windows-x64-installer"
#endif

[Setup]
AppId={{6E822056-33C9-4E27-8F1C-E8FDBD3C12A4}
AppName=Chiripy Chat
AppVersion={#AppVersion}
AppVerName=Chiripy Chat {#AppVersion}
AppPublisher=Coffee Run Hobby
AppPublisherURL=https://github.com/coffeerunhobby/chiripy
AppSupportURL=https://github.com/coffeerunhobby/chiripy/issues
AppUpdatesURL=https://github.com/coffeerunhobby/chiripy/releases
VersionInfoVersion={#AppVersion}
; OBS 30+ loads machine-wide plugins from here: <dir>\bin\64bit\<name>.dll
; plus <dir>\data. Fixed location, so no directory page.
DefaultDirName={commonappdata}\obs-studio\plugins\chiripy
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; OBS Studio 31 and Qt 6 need Windows 10 or 11; on older Windows the setup
; explains that (see [Messages]) instead of installing something that can
; never load.
MinVersion=10.0
; Restart Manager: if OBS has chiripy.dll loaded, ask to close it.
CloseApplications=yes
RestartApplications=no
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
UninstallDisplayName=Chiripy Chat (OBS Studio plugin)

[Messages]
WindowsVersionNotSupported=Chiripy is a plugin for OBS Studio 31 or later, and OBS Studio 31 needs Windows 10 or Windows 11. This computer runs an older version of Windows, so there is no OBS for Chiripy to plug into.

[InstallDelete]
; Upgrade = clean replace: remove whatever an older version put here.
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\data"

[Files]
; The .pdb (debug symbols) stays out of the installer.
Source: "{#SourceDir}\chiripy\bin\64bit\chiripy.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#SourceDir}\chiripy\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
; Settings and the sealed API key live in %APPDATA%\obs-studio\plugin_config
; and are deliberately kept; only the plugin folder goes.
Type: dirifempty; Name: "{app}"

[Code]
// A soft check: warn when OBS Studio does not look installed, but let the
// user continue (portable OBS setups have no registry key). Suppressible, so
// /SUPPRESSMSGBOXES in silent installs takes the default: continue.
function InitializeSetup(): Boolean;
begin
  Result := True;
  if not RegKeyExists(HKLM64, 'SOFTWARE\OBS Studio') then
    Result := SuppressibleMsgBox(
      'OBS Studio does not appear to be installed on this computer.' + #13#10#13#10 +
      'Chiripy is a plugin for OBS Studio 31 or later. Install it anyway?',
      mbConfirmation, MB_YESNO, IDYES) = IDYES;
end;
