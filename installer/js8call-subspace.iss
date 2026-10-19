#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppName=Subspace Edition
AppVersion={#AppVersion}
AppPublisher=Subspace Edition
DefaultDirName={localappdata}\Programs\Subspace Edition
DefaultGroupName=Subspace Edition
UninstallDisplayIcon={app}\JS8Call.exe
OutputBaseFilename=Subspace-Edition_{#AppVersion}_win64
OutputDir=Output
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
Compression=lzma2
SolidCompression=yes

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Files]
Source: "..\build\JS8Call\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Subspace Edition"; Filename: "{app}\JS8Call.exe"
Name: "{group}\Uninstall Subspace Edition"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Subspace Edition"; Filename: "{app}\JS8Call.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\JS8Call.exe"; Description: "Launch Subspace Edition"; Flags: nowait postinstall skipifsilent
