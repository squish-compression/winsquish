; ==========================================================================
; WinSquish installer -- system-wide (default) or per-user, your choice.
;
; At startup it asks "Install for all users" (default, system-wide, one UAC
; prompt) or "Install for me only" (per-user, no admin). Inno's {auto*}
; constants resolve the install location and shortcuts to match.
;
; It installs the self-contained WinSquish build (WinSquish.exe + the native
; squish.dll + the WPF native runtime files from ..\publish) into
; {autopf}\WinSquish, then registers the .sqsh/.sq file type and the Explorer
; context-menu entries by invoking the app's own registration:
;   "WinSquish.exe --register [--allusers] --quiet"
; so HKLM (all-users) or HKCU (per-user) has one source of truth. The
; uninstaller unregisters before the files are removed.
;
; Build:  installer\build-installer.bat     (publishes, then runs ISCC)
; Requires Inno Setup 6.3+ (x64compatible architecture id).
; ==========================================================================

#define AppName "WinSquish"
#define AppVersion "1.0.0"
#define AppPublisher "Paige Julianne Sullivan"
#define AppExe "WinSquish.exe"
#define AppUrl "https://github.com/paigejulianne/winsquish"

[Setup]
AppId={{6D9B2F41-8C3E-4A7D-9F2B-1E5C7A9D4B60}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
AppCopyright=Copyright (C) 2026 Paige Julianne Sullivan. GPL-3.0.
VersionInfoVersion=1.0.0.0
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\build
OutputBaseFilename=winsquish-setup
SetupIconFile=..\src\assets\winsquish.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Broadcast SHCNE_ASSOCCHANGED after install/uninstall so Explorer notices the
; new (or removed) association without a logoff.
ChangesAssociations=yes
; Offer both scopes at startup; default all-users (one UAC elevation).
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1607+ (the app calls modern shell/DPI APIs).
MinVersion=10.0
; Restart Manager closes a running WinSquish so its files aren't locked.
CloseApplications=yes
RestartApplications=no
; Authenticode signing. build-installer.bat passes /DSign and defines the
; "winsquishsign" SignTool (a signtool.exe wrapper) when a signing certificate
; is configured; without it the installer builds unsigned. SignedUninstaller
; makes Inno sign the uninstaller it embeds, too.
#ifdef Sign
SignTool=winsquishsign
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; The entire self-contained publish output (exe + squish.dll + WPF natives).
Source: "..\publish\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\LICENSE";   DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}";           Filename: "{app}\{#AppExe}"; Comment: "File-manager for SQUISH archives"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";     Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
; Register the file type + context-menu verbs, scoped to the install mode.
Filename: "{app}\{#AppExe}"; Parameters: "--register --allusers --quiet"; StatusMsg: "Registering the .sqsh file type and context-menu entries (all users)..."; Check: IsAdminInstallMode
Filename: "{app}\{#AppExe}"; Parameters: "--register --quiet"; StatusMsg: "Registering the .sqsh file type and context-menu entries..."; Flags: runasoriginaluser; Check: not IsAdminInstallMode
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Unregister BEFORE the files are removed -- the exe must still exist to run.
Filename: "{app}\{#AppExe}"; Parameters: "--unregister --allusers --quiet"; RunOnceId: "UnregisterShellAll"; Check: IsAdminInstallMode
Filename: "{app}\{#AppExe}"; Parameters: "--unregister --quiet"; RunOnceId: "UnregisterShell"; Check: not IsAdminInstallMode

[CustomMessages]
CreateDesktopIcon=Create a &desktop shortcut
AdditionalIcons=Additional shortcuts:
LaunchProgram=Launch %1
