[Setup]
AppName=Tray Icon Promoter
AppVersion=1.0
AppPublisher=jimnastic
AppPublisherURL=https://github.com/jimnastic89/TrayIconPromoter
DefaultDirName={pf64}\TrayIconPromoter
PrivilegesRequired=admin
OutputBaseFilename=TrayIconPromoterSetup
Compression=lzma
SolidCompression=yes

[Files]
Source: "x64\Release\TrayIconPromoter.exe"; DestDir: "{app}"

[Run]
Filename: "{app}\TrayIconPromoter.exe"; Parameters: "/install"; Flags: runhidden waituntilterminated

[UninstallRun]
Filename: "{app}\TrayIconPromoter.exe"; Parameters: "/uninstall"; Flags: runhidden waituntilterminated