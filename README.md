# Tray Icon Promoter
A lightweight Windows service to show all icons in the taskbar tray.

I couldn't find a way to show all tray icons in Windows 11 - the old Windows 10 tick box has long since been removed, and no workarounds I found online worked.

This works by creating a background service that monitors subkeys in HKEY_CURRENT_USER\Control Panel\NotifyIconSettings. Each tray icon has it's own subkey in here with a DWORD value "IsPromoted" that controls if the icon is visible (1) or hidden (0). The services ensures that all of these values stay set to 1.

Executable built using Visual Studio 2026, and installer is built with Inno Setup using the installer.iss script.
