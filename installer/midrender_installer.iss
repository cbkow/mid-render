; MidRender Installer Script for Inno Setup 6
; https://jrsoftware.org/isinfo.php

#define MyAppName "MidRender"
#define MyAppVersion "0.2.7"
#define MyAppPublisher "cbkow"
#define MyAppURL "https://github.com/cbkow/midrender"
#define MyAppExeName "midrender.exe"
#define MyAgentExeName "mr-agent.exe"

[Setup]
; App information
AppId={{A9F2D4E7-8B1C-4E3A-B5D9-6C2F8A0E3B7D}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
AppCopyright=Copyright (C) 2025-2026 {#MyAppPublisher}

; Installation directories
DefaultDirName={autopf}\MidRender
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes

; Licensing
LicenseFile=..\LICENSE

; Output settings
OutputDir=.
OutputBaseFilename=midrender-setup-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes

; Visual settings
SetupIconFile=..\resources\icons\midrender.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
WizardStyle=modern

; System requirements
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=10.0.17763

; Uninstaller settings
UninstallDisplayName={#MyAppName}
UninstallFilesDir={app}\uninstall

; Miscellaneous
AllowNoIcons=yes
DisableWelcomePage=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Full installation (recommended)"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "core"; Description: "Core application files (Monitor + Agent)"; Types: full custom; Flags: fixed
Name: "plugins"; Description: "DCC plugins for render submission"
Name: "plugins\blender"; Description: "Blender addon (MidRender.py)"; Types: full
Name: "plugins\cinema4d"; Description: "Cinema 4D script (MidRender.py)"; Types: full
Name: "plugins\aftereffects"; Description: "After Effects script (MidRender.jsx)"; Types: full
Name: "shortcuts"; Description: "Create shortcuts"
Name: "shortcuts\desktop"; Description: "Create desktop shortcut"; Types: full
Name: "shortcuts\startmenu"; Description: "Create Start Menu shortcuts"; Types: full; Flags: fixed
Name: "shortcuts\startup"; Description: "Launch at Windows startup (minimized to tray)"; Types: full

[Tasks]
Name: "cleanconfig"; Description: "Remove user configuration (%LOCALAPPDATA%\MidRender\config.json) - NOT RECOMMENDED"; GroupDescription: "User data cleanup:"; Flags: unchecked
Name: "cleanall"; Description: "Remove ALL user data (%LOCALAPPDATA%\MidRender\) - NOT RECOMMENDED"; GroupDescription: "User data cleanup:"; Flags: unchecked
Name: "launchafter"; Description: "Launch {#MyAppName} after installation"; GroupDescription: "Post-installation:"; Flags: checkedonce

[Files]
; Main executables
Source: "..\build\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "..\build\Release\{#MyAgentExeName}"; DestDir: "{app}"; Flags: ignoreversion; Components: core

; Resources — fonts
Source: "..\build\Release\resources\fonts\*"; DestDir: "{app}\resources\fonts"; Flags: ignoreversion; Components: core

; Resources — icons (exclude source artwork)
Source: "..\build\Release\resources\icons\midrender.ico"; DestDir: "{app}\resources\icons"; Flags: ignoreversion; Components: core
Source: "..\build\Release\resources\icons\tray_*.ico"; DestDir: "{app}\resources\icons"; Flags: ignoreversion; Components: core

; Resources — templates (top-level + plugins subfolder)
Source: "..\build\Release\resources\templates\*.json"; DestDir: "{app}\resources\templates"; Flags: ignoreversion; Components: core
Source: "..\build\Release\resources\templates\plugins\*"; DestDir: "{app}\resources\templates\plugins"; Flags: ignoreversion; Components: core

; DCC plugins
Source: "..\build\Release\resources\plugins\blender\*"; DestDir: "{app}\resources\plugins\blender"; Flags: ignoreversion; Components: plugins\blender
Source: "..\build\Release\resources\plugins\cinema4d\*"; DestDir: "{app}\resources\plugins\cinema4d"; Flags: ignoreversion; Components: plugins\cinema4d
Source: "..\build\Release\resources\plugins\afterEffects\*"; DestDir: "{app}\resources\plugins\afterEffects"; Flags: ignoreversion; Components: plugins\aftereffects

; Documentation
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion; Components: core

[Icons]
; Start Menu shortcuts
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Components: shortcuts\startmenu
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"; Components: shortcuts\startmenu

; Desktop shortcut
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Components: shortcuts\desktop

; Windows Startup shortcut (launches minimized to tray)
Name: "{userstartup}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Parameters: "--minimized"; Components: shortcuts\startup

[Registry]
; App User Model ID (for Windows 11 taskbar/start menu)
Root: HKLM; Subkey: "Software\Classes\Applications\{#MyAppExeName}"; ValueType: string; ValueName: "AppUserModelID"; ValueData: "cbkow.midrender"; Flags: uninsdeletekey

; Uninstall registry info
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Uninstall\{{A9F2D4E7-8B1C-4E3A-B5D9-6C2F8A0E3B7D}_is1"; ValueType: string; ValueName: "DisplayName"; ValueData: "{#MyAppName}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Uninstall\{{A9F2D4E7-8B1C-4E3A-B5D9-6C2F8A0E3B7D}_is1"; ValueType: string; ValueName: "DisplayIcon"; ValueData: "{app}\{#MyAppExeName},0"
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Uninstall\{{A9F2D4E7-8B1C-4E3A-B5D9-6C2F8A0E3B7D}_is1"; ValueType: string; ValueName: "DisplayVersion"; ValueData: "{#MyAppVersion}"
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Uninstall\{{A9F2D4E7-8B1C-4E3A-B5D9-6C2F8A0E3B7D}_is1"; ValueType: string; ValueName: "Publisher"; ValueData: "{#MyAppPublisher}"
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Uninstall\{{A9F2D4E7-8B1C-4E3A-B5D9-6C2F8A0E3B7D}_is1"; ValueType: string; ValueName: "URLInfoAbout"; ValueData: "{#MyAppURL}"

[Code]
// Pascal Script for install/uninstall logic

// Pre-install cleanup and post-install actions
procedure CurStepChanged(CurStep: TSetupStep);
var
  LocalAppData: String;
  ConfigFile: String;
  ResultCode: Integer;
begin
  if CurStep = ssInstall then
  begin
    // Always clean old program files before installing (upgrade-safe)
    if DirExists(ExpandConstant('{app}')) then
    begin
      Log('Deleting old program files before install: ' + ExpandConstant('{app}'));
      DelTree(ExpandConstant('{app}'), True, True, True);
    end;

    LocalAppData := ExpandConstant('{localappdata}\MidRender');

    // Clean ALL user data if selected (takes precedence)
    if WizardIsTaskSelected('cleanall') then
    begin
      if DirExists(LocalAppData) then
      begin
        Log('Cleaning ALL user data: ' + LocalAppData);
        DelTree(LocalAppData, True, True, True);
      end;
    end
    else if WizardIsTaskSelected('cleanconfig') then
    begin
      ConfigFile := LocalAppData + '\config.json';
      if FileExists(ConfigFile) then
      begin
        Log('Deleting config file: ' + ConfigFile);
        DeleteFile(ConfigFile);
      end;
    end;
  end;

  if CurStep = ssPostInstall then
  begin
    // Add Windows Firewall rules for HTTP (TCP) and UDP multicast
    Exec('netsh', 'advfirewall firewall delete rule name="MidRender"',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec('netsh', 'advfirewall firewall delete rule name="MidRender UDP"',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec('netsh', 'advfirewall firewall add rule name="MidRender" dir=in action=allow protocol=tcp localport=8420 enable=yes',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec('netsh', 'advfirewall firewall add rule name="MidRender UDP" dir=in action=allow protocol=udp localport=4243 enable=yes',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Log('Firewall rules added for TCP 8420 + UDP 4243');
  end;
end;

// Uninstall — prompt to delete user data
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  UserDataDir: String;
  Response: Integer;
  ResultCode: Integer;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    // Remove firewall rules
    Exec('netsh', 'advfirewall firewall delete rule name="MidRender"',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec('netsh', 'advfirewall firewall delete rule name="MidRender UDP"',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    UserDataDir := ExpandConstant('{localappdata}\MidRender');
    if DirExists(UserDataDir) then
    begin
      Response := MsgBox('Do you want to delete your MidRender user data and configuration?' + #13#10 +
                         'Location: ' + UserDataDir + #13#10#13#10 +
                         'This includes:' + #13#10 +
                         '  - config.json (sync root path, node settings, preferences)' + #13#10 +
                         '  - Node identity files' + #13#10#13#10 +
                         'Choose "Yes" to delete all user data (clean uninstall)' + #13#10 +
                         'Choose "No" to keep your data for future installations (RECOMMENDED)',
                         mbConfirmation, MB_YESNO);
      if Response = IDYES then
      begin
        Log('User chose to delete all user data during uninstall');
        DelTree(UserDataDir, True, True, True);
      end
      else
      begin
        Log('User chose to keep user data during uninstall');
      end;
    end;
  end;
end;

[Run]
; Launch application after installation if task is selected
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent; Tasks: launchafter

[UninstallDelete]
; Clean up any runtime-generated files not tracked by installer
Type: filesandordirs; Name: "{app}\logs"
Type: filesandordirs; Name: "{app}\cache"
; Remove startup shortcut on uninstall
Type: files; Name: "{userstartup}\{#MyAppName}.lnk"

[Messages]
; Custom messages
WelcomeLabel2=This will install [name/ver] on your computer.%n%nMidRender is a render farm coordinator for freelancers and small teams.%n%nIt is recommended that you close all other applications before continuing.
FinishedLabel=Setup has finished installing [name] on your computer.%n%nThe application may be launched by selecting the installed shortcuts.
