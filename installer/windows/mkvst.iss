; Inno-Setup-Skript fuer mkvst0_4 (Windows, 64 Bit)
; Wird vom Workflow .github/workflows/installer.yml aufgerufen:
;   ISCC.exe /DAppVersion=0.5.1 /DBuildDir=<...>\build\Mkvst04_artefacts\Release mkvst.iss

#ifndef AppVersion
  #define AppVersion "0.5.1"
#endif
#ifndef BuildDir
  #define BuildDir "..\..\build\Mkvst04_artefacts\Release"
#endif
#define AppName "mkvst0_4"

[Setup]
AppId={{7E3C2A41-5B8D-4F6A-9C1E-0D4B2F8A6E13}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Sarin
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputBaseFilename={#AppName}-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "de"; MessagesFile: "compiler:Languages\German.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Alles"
Name: "custom"; Description: "Auswahl"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3-Plug-in"; Types: full custom
Name: "standalone"; Description: "Standalone-App"; Types: full

[Files]
Source: "{#BuildDir}\VST3\{#AppName}.vst3\*"; DestDir: "{commoncf64}\VST3\{#AppName}.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\Standalone\{#AppName}.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppName}.exe"; Components: standalone

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\{#AppName}.vst3"
