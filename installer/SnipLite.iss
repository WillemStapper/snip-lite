#define MyIconFile "assets\snip_lite.ico"
#define MyAppName "SnipLite"
#define MyAppVersion "1.0.0"
#define MyAppExeName "snip_lite.exe"
#define MyDistDir "E:\GitHub\repos\WillemStapper\snip-lite\dist\SnipLite-1.0.0-win64\"

[Setup]
AppId={{7B2C6B7F-9B7E-4B2E-9C7C-1C2F6B1A9B11}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=E:\GitHub\repos\WillemStapper\snip-lite\dist\
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-Setup
SetupIconFile={#MyIconFile}
UninstallDisplayIcon={app}\snip_lite.exe
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
Source: "{#MyDistDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyDistDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion; Check: FileExists(ExpandConstant('{#MyDistDir}\README.md'))

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent