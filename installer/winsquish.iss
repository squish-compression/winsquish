; ==========================================================================
; WinSquish installer -- per-user, no administrator rights required.
;
; Builds a single setup.exe that:
;   * installs winsquish.exe into %LOCALAPPDATA%\Programs\WinSquish
;   * registers the .sq file type + Explorer context-menu entries by
;     invoking "winsquish.exe --register --quiet" -- the app's own per-user
;     HKCU registration, so the registry layout has one source of truth
;   * creates a Start Menu shortcut (and an optional desktop shortcut)
;   * adds an Add/Remove Programs entry whose uninstaller unregisters the
;     shell integration before deleting the files
;
; Everything lives under HKEY_CURRENT_USER and the per-user profile, so the
; installer never prompts for elevation -- matching the app's design.
;
; Requires Inno Setup 6.3 or later (uses the x64compatible architecture id).
; Compile with:  installer\build-installer.bat   (or: ISCC winsquish.iss)
;
; Authenticode signing is optional and off by default. build-installer.bat
; turns it on (signing both setup.exe and the uninstaller) when
; sign-metadata.json exists, reusing the same Azure Trusted Signing setup as
; build.bat -- it passes /DSIGN plus a /Swinsquish=... sign command to ISCC.
; ==========================================================================

#define AppName "WinSquish"
#define AppVersion "1.0.0"
#define AppPublisher "Paige Julianne Sullivan"
#define AppExe "winsquish.exe"
#define AppUrl "https://github.com/paigejulianne/winsquish"

[Setup]
; A stable, WinSquish-specific AppId -- keeps upgrades and the ARP entry
; tracked across versions. Do not change it between releases.
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
SetupIconFile=..\src\winsquish.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Broadcast SHCNE_ASSOCCHANGED after install/uninstall so Explorer notices
; the new (or removed) .sq association without a logoff.
ChangesAssociations=yes
; Per-user install: no UAC prompt, everything under the user's profile / HKCU.
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; The app calls GetDpiForSystem (Windows 10 1607+).
MinVersion=10.0
; Use Restart Manager to close a running WinSquish so its exe isn't locked.
CloseApplications=yes
RestartApplications=no
; Optional Authenticode signing, enabled by passing /DSIGN to ISCC together
; with a /Swinsquish=... sign command (build-installer.bat wires this up).
; Signs both setup.exe and the extracted uninstaller.
#ifdef SIGN
SignTool=winsquish
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";         DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\README.md";       DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}";           Filename: "{app}\{#AppExe}"; Comment: "GUI for the SQUISH compressor"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";     Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
; Register the .sq file type + context-menu verbs for the current user.
; runasoriginaluser: register for the real user even if setup was elevated.
Filename: "{app}\{#AppExe}"; Parameters: "--register --quiet"; StatusMsg: "Registering the .sq file type and context-menu entries..."; Flags: runasoriginaluser
; Offer to launch WinSquish from the final wizard page.
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Unregister BEFORE the files are removed -- the exe must still exist to run.
; --unregister only drops the .sq mapping if it still points at us.
Filename: "{app}\{#AppExe}"; Parameters: "--unregister --quiet"; RunOnceId: "UnregisterShell"

[CustomMessages]
CreateDesktopIcon=Create a &desktop shortcut
AdditionalIcons=Additional shortcuts:
LaunchProgram=Launch %1
