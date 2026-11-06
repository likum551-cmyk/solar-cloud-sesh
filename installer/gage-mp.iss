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
; ui\ (the in-game UI) and launcher-assets\ (the launcher UI's footage and stills; the exe serves
; only what its manifest.json lists) are replaced whole, so no file from an older version lingers.
; They are NOT deleted here: [InstallDelete] runs before [Files], and a setup that then aborts
; keeps no rollback copy. 0.5.1 and 0.5.2 (2026-11-06) aborted on gage_launch_shim.dll, which the
; Rockstar Launcher.exe still had loaded, after the exe was already replaced: the launcher was
; left with no footage and a version that said it was up to date. [Files] stages everything as
; <name>.new and CommitStaged (after the last [Files] entry) swaps it in; only staging leftovers of an earlier
; setup are removed here.
Type: filesandordirs; Name: "{app}\ui.new"
Type: filesandordirs; Name: "{app}\launcher-assets.new"
Type: files; Name: "{app}\{#AppExe}.new"
Type: files; Name: "{app}\gageinjector.dll.new"
Type: files; Name: "{app}\gage_loader.dll.new"
Type: files; Name: "{app}\gage_mp.dll.new"
Type: files; Name: "{app}\gage_launch_shim.dll.new"
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
; Everything a release replaces is staged as <name>.new and swapped in by CommitStaged, all or
; none, once every file is on disk: an aborted setup does not roll back a file it already
; replaced, and the exe carries the version the updater compares. A setup that stops or cannot
; swap leaves the previous version whole (exe, chain DLLs whose hashes it bakes, footage), and
; the updater installs the release again on its next check. A DLL another process still has
; loaded (the shim stays in the Rockstar Launcher.exe after the game) cannot be overwritten or
; deleted but can be renamed, so the swap never stops on one.
Source: "{#BinDir}\ui\*"; DestDir: "{app}\ui.new"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BinDir}\launcher-assets\*"; DestDir: "{app}\launcher-assets.new"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BinDir}\gageinjector.dll"; DestDir: "{app}"; DestName: "gageinjector.dll.new"; Flags: ignoreversion
Source: "{#BinDir}\gage_loader.dll"; DestDir: "{app}"; DestName: "gage_loader.dll.new"; Flags: ignoreversion
Source: "{#BinDir}\gage_mp.dll"; DestDir: "{app}"; DestName: "gage_mp.dll.new"; Flags: ignoreversion
Source: "{#BinDir}\gage_launch_shim.dll"; DestDir: "{app}"; DestName: "gage_launch_shim.dll.new"; Flags: ignoreversion
; The commit runs after this, the LAST [Files] entry, and not at ssPostInstall: Setup runs the
; [Run] entries (the updater's restart) before ssPostInstall, which would start the old exe.
Source: "{#BinDir}\{#AppExe}"; DestDir: "{app}"; DestName: "{#AppExe}.new"; Flags: ignoreversion; AfterInstall: CommitStaged

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
; what CommitStaged swapped in (the uninstall log names only the <name>.new staging paths), and
; its leftovers: <name>.new of a failed commit, <name>.old<n> of a file that was still in use
Type: files; Name: "{app}\{#AppExe}"
Type: files; Name: "{app}\{#AppExe}.new"
Type: files; Name: "{app}\{#AppExe}.old*"
Type: files; Name: "{app}\gageinjector.dll"
Type: files; Name: "{app}\gage_loader.dll"
Type: files; Name: "{app}\gage_mp.dll"
Type: files; Name: "{app}\gage_launch_shim.dll"
Type: files; Name: "{app}\*.dll.new"
Type: files; Name: "{app}\*.dll.old*"
Type: filesandordirs; Name: "{app}\ui"
Type: filesandordirs; Name: "{app}\launcher-assets"
Type: filesandordirs; Name: "{app}\ui.new"
Type: filesandordirs; Name: "{app}\launcher-assets.new"
Type: filesandordirs; Name: "{app}\ui.old*"
Type: filesandordirs; Name: "{app}\launcher-assets.old*"
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

// The items CommitStaged has swapped so far: {app}\<name> and where its previous copy went
// ('' = there was none).
var
  SwappedFinal, SwappedOld: TArrayOfString;

function Exists(const P: String): Boolean;
begin
  Result := DirExists(P) or FileExists(P);
end;

// Deletes a file or a whole folder; False when something in it is in use.
function Remove(const P: String): Boolean;
begin
  if DirExists(P) then
    Result := DelTree(P, True, True, True)
  else
    Result := DeleteFile(P);
end;

// {app}\<Name>.new (a file or a folder staged by [Files]) replaces {app}\<Name>. The current one
// is renamed aside whole, never deleted in place, so it can be put back; a rename also works on
// a DLL another process has loaded (only a handle opened without delete sharing stops it).
function SwapIn(const Name: String): Boolean;
var
  Final, Staged, Old: String;
  N: Integer;
begin
  Result := False;
  Final := ExpandConstant('{app}\' + Name);
  Staged := Final + '.new';
  if not Exists(Staged) then
  begin
    Log('commit: ' + Staged + ' is missing: ' + Name + ' was not staged');
    Exit;
  end;
  Old := '';
  if Exists(Final) then
  begin
    // a leftover of an earlier setup goes first; one still in use stays under its own name
    N := 1;
    Old := Final + '.old1';
    while Exists(Old) and not Remove(Old) and (N < 100) do
    begin
      N := N + 1;
      Old := Final + '.old' + IntToStr(N);
    end;
    if not RenameFile(Final, Old) then
    begin
      Log('commit: could not move ' + Final + ' aside (something in it is in use)');
      Exit;
    end;
  end;
  if not RenameFile(Staged, Final) then
  begin
    Log('commit: could not move ' + Staged + ' to ' + Final);
    if (Old <> '') and not RenameFile(Old, Final) then Log('commit: could not put ' + Old + ' back');
    Exit;
  end;
  SetArrayLength(SwappedFinal, GetArrayLength(SwappedFinal) + 1);
  SetArrayLength(SwappedOld, GetArrayLength(SwappedOld) + 1);
  SwappedFinal[GetArrayLength(SwappedFinal) - 1] := Final;
  SwappedOld[GetArrayLength(SwappedOld) - 1] := Old;
  Log('commit: swapped in ' + Final);
  Result := True;
end;

// Puts back what SwapIn swapped, newest first; the new copies go back to <name>.new, which the
// next setup's [InstallDelete] removes.
procedure UndoSwaps();
var
  I: Integer;
begin
  for I := GetArrayLength(SwappedFinal) - 1 downto 0 do
  begin
    if not RenameFile(SwappedFinal[I], SwappedFinal[I] + '.new') then
      Log('undo: could not move ' + SwappedFinal[I] + ' back to staging')
    else if (SwappedOld[I] <> '') and not RenameFile(SwappedOld[I], SwappedFinal[I]) then
      Log('undo: could not put ' + SwappedOld[I] + ' back')
    else
      Log('undo: restored ' + SwappedFinal[I]);
  end;
end;

// All or none: the launcher UI's footage (launcher-assets), the in-game UI (ui), the chain DLLs
// and the exe that expects them (it bakes the DLLs' hashes). The exe goes last: it carries the
// version, so when anything fails the previous version stays whole and the launcher's updater
// installs this release again. An old copy still in use (a DLL loaded elsewhere) stays as
// <name>.old<n> until a later setup can delete it.
procedure CommitStaged();
var
  I: Integer;
begin
  SetArrayLength(SwappedFinal, 0);
  SetArrayLength(SwappedOld, 0);
  if SwapIn('launcher-assets') and SwapIn('ui') and SwapIn('gage_launch_shim.dll') and
     SwapIn('gageinjector.dll') and SwapIn('gage_loader.dll') and SwapIn('gage_mp.dll') and
     SwapIn('{#AppExe}') then
  begin
    Log('commit: version {#AppVersion} is in place');
    for I := 0 to GetArrayLength(SwappedOld) - 1 do
      if (SwappedOld[I] <> '') and not Remove(SwappedOld[I]) then
        Log('commit: could not delete ' + SwappedOld[I] + '; the next setup removes it');
  end
  else
  begin
    UndoSwaps();
    Log('commit FAILED: the previous version stays; the launcher installs {#AppVersion} again on its next update check');
  end;
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
