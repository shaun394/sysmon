[Setup]
AppName=SysMon
AppVersion=1.0.0
DefaultDirName={pf}\SysMon
DefaultGroupName=SysMon
OutputBaseFilename=SysMonSetup
Compression=lzma
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "..\gui\dist\SysMon\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\SysMon"; Filename: "{app}\SysMon.exe"
Name: "{commondesktop}\SysMon"; Filename: "{app}\SysMon.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"

[Run]
Filename: "{app}\SysMon.exe"; Description: "Launch SysMon"; Flags: nowait postinstall skipifsilent
