@echo off
reg add "HKCR\.md" /ve /t REG_SZ /d "ObsidianMD" /f
reg add "HKCR\ObsidianMD" /ve /t REG_SZ /d "Markdown File" /f
reg add "HKCR\ObsidianMD\shell\open\command" /ve /t REG_SZ /d "\"C:\Users\azt12\OneDrive\Documents\Computing\All Projects\Windows OS Work\Right-Click Menu API\WORKING\build\open_md_in_obsidian.bat\" \"%%%%1\"" /f
echo Done. Restart explorer to apply.
