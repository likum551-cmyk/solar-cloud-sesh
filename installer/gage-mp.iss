; Solar RP setup (Inno Setup 6.6+; internal file and id names stay GAGE-MP). Build it with installer/build-installer.sh,
; which reads the version from apps/launcher/gage_gui.cpp (kClientVersion) and
; points BinDir at the built launcher.
;
; Per-user install into %LOCALAPPDATA%\Programs\Solar RP (a new install), no admin prompt: the
; launcher writes gage_mp.cfg, gage.log and its WebView2 data next to the exe,
; so the install folder must stay writable by the player.
;
; Upgrade from GAGE-MP (0.3.x and older): the AppId below is unchanged, so Setup finds the
; old install, keeps its folder (UsePreviousAppDir; the player's gage_mp.cfg, gage.log and
; WebView2 profile stay where they are), replaces GAGE-MP.exe with SolarLauncher.exe and
; renames the shortcuts ([InstallDelete] removes the old ones). The launcher's mutex, the
; registry key HKCU\Software\GAGE-MP and the DLL names are unchanged on purpose.

#ifndef AppVersion
  #error AppVersion is not defined: build with installer/build-installer.sh
#endif
#ifndef BinDir
  #define BinDir "..\build\win\bin"
#endif

#define AppName "Solar RP"
#define LauncherName "Solar Launcher"
#define AppExe "SolarLauncher.exe"
; the launcher exe and shortcut names before the rename (removed on upgrade)
#define OldAppExe "GAGE-MP.exe"
#define OldShortcut "GAGE-MP.lnk"
; the launcher's single-instance mutex (gage_gui.cpp kSingleInstanceMutex);
; overridable only so a test install can run beside a live launcher
#ifndef AppMutexName
  #define AppMutexName "Local\GAGE-MP.Launcher.SingleInstance"
#endif
; Evergreen WebView2 runtime client id (webview2_loader.cpp, stable channel)
#define WebView2Client "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"

[Setup]
AppId={{D6409B6A-D83B-4F00-869D-02DC833AD2AB}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName}
UsePreviousAppDir=yes
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Setup
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
DisableReadyPage=yes
DisableWelcomePage=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; a running launcher (often only in the tray) is detected by its mutex, and
; setup asks for it to be closed instead of force-closing it
AppMutex={#AppMutexName}
CloseApplications=no
SetupIconFile=..\apps\launcher\res\gage-mp.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern dark
WizardImageFile=art\out\wizard-*.png
WizardSmallImageFile=art\out\mark-*.png
WizardImageBackColor=#0B0B0D
WizardSmallImageBackColor=#0B0B0D
OutputDir=..\build\win\installer
OutputBaseFilename=SolarRP-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel2=This will install [name/ver] on your computer.%n%nSolar RP finds multiplayer servers for Grand Theft Auto V and connects you to them. You need GTA V installed to play.

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[InstallDelete]
; the in-game UI is replaced whole, so no file from an older version lingers
Type: filesandordirs; Name: "{app}\ui"
; the launcher UI's 3D set too: the exe serves only what its manifest.json lists
Type: filesandordirs; Name: "{app}\launcher-assets"
; migration: earlier GAGE-MP versions shipped a bundled engine runtime in rage\
; and an anti-cheat shim, gage_eac.dll. Neither is part of Solar RP any more, so an
; upgrade removes them
Type: filesandordirs; Name: "{app}\rage"
Type: files; Name: "{app}\gage_eac.dll"
; migration: the launcher was GAGE-MP.exe with GAGE-MP shortcuts before the rename to
; SolarLauncher.exe / Solar Launcher; an upgrade removes the old exe and both old shortcuts
Type: files; Name: "{app}\{#OldAppExe}"
Type: files; Name: "{autoprograms}\{#OldShortcut}"
Type: files; Name: "{autodesktop}\{#OldShortcut}"

[Files]
Source: "{#BinDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\gageinjector.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\gage_loader.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\gage_mp.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\gage_launch_shim.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\ui\*"; DestDir: "{app}\ui"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BinDir}\launcher-assets\*"; DestDir: "{app}\launcher-assets"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#LauncherName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#LauncherName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#LauncherName}}"; Flags: nowait postinstall skipifsilent shellexec
; A silent install the launcher's own updater started (it passes /GAGEUPDATE, see apps/launcher/updater.cpp
; InstallerCommandLine) starts the new version when it is done. A hand-run silent install carries no such
; switch, so it still starts nothing.
Filename: "{app}\{#AppExe}"; Flags: nowait shellexec; Check: IsGageUpdate

[UninstallDelete]
; what the launcher writes next to itself: settings copy (+ its atomic-write
; temp files), log (+ the rotated one) and the WebView2 profile, plus what older
; versions left behind (client_resources\ server downloads, rage\ runtime).
; Settings in HKCU\Software\GAGE-MP are kept, so a reinstall remembers the game
; folder. The pre-rename exe and shortcuts go too, in case an upgrade could not remove them.
Type: files; Name: "{app}\{#OldAppExe}"
Type: files; Name: "{autoprograms}\{#OldShortcut}"
Type: files; Name: "{autodesktop}\{#OldShortcut}"
Type: files; Name: "{app}\gage_mp.cfg*"
Type: files; Name: "{app}\gage.log*"
Type: filesandordirs; Name: "{app}\webview2"
Type: filesandordirs; Name: "{app}\client_resources"
Type: filesandordirs; Name: "{app}\rage"
Type: dirifempty; Name: "{app}"

[Code]
function WebView2Installed(): Boolean;
var
  V: String;
begin
  // Microsoft's documented check: a non-empty pv other than 0.0.0.0, per-machine
  // (32-bit view) or per-user
  Result :=
    (RegQueryStringValue(HKLM32, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{#WebView2Client}', 'pv', V) and (V <> '') and (V <> '0.0.0.0')) or
    (RegQueryStringValue(HKCU, 'Software\Microsoft\EdgeUpdate\Clients\{#WebView2Client}', 'pv', V) and (V <> '') and (V <> '0.0.0.0'));
end;

// True when the launcher's updater started this setup: it passes /GAGEUPDATE (a custom switch;
// Setup ignores switches it does not know).
function IsGageUpdate(): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/GAGEUPDATE') = 0 then
    begin
      Result := True;
      Exit;
    end;
end;

// The updater starts Setup, then exits the launcher about 1.5 s later. Wait for it to be gone
// (its single-instance mutex) instead of letting the AppMutex check below turn that moment into an
// aborted silent update. Gives up after 30 s; Setup's own check then applies.
procedure WaitForLauncherExit();
var
  Waited: Integer;
begin
  Waited := 0;
  while CheckForMutexes('{#AppMutexName}') and (Waited < 30000) do
  begin
    Sleep(250);
    Waited := Waited + 250;
  end;
  Log('launcher mutex gone after ' + IntToStr(Waited) + ' ms');
end;

function InitializeSetup(): Boolean;
var
  Code: Integer;
begin
  Result := True;
  if IsGageUpdate() then WaitForLauncherExit();
  if WebView2Installed() then Exit;
  Log('WebView2 runtime not found');
  if SuppressibleMsgBox(
       'Solar RP needs the Microsoft Edge WebView2 Runtime, which was not found on this PC.' + #13#10#13#10 +
       'Open the download page now? Setup will continue; install the runtime before starting Solar Launcher.',
       mbConfirmation, MB_YESNO, IDNO) = IDYES then
    ShellExec('open', 'https://go.microsoft.com/fwlink/p/?LinkId=2124703', '', '', SW_SHOWNORMAL, ewNoWait, Code);
end;
