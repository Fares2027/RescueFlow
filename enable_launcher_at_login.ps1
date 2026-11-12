# Optional: run the launcher automatically when you sign in to Windows.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$python = (& py -c 'import sys; print(sys.executable)').Trim()
if (-not (Test-Path $python)) { throw 'Python installation not found. Run py --version.' }
$pythonw = Join-Path (Split-Path -Parent $python) 'pythonw.exe'
if (-not (Test-Path $pythonw)) { throw 'pythonw.exe not found. Use py rescueflow_laptop_agent.py manually.' }
$startup = [Environment]::GetFolderPath('Startup')
$shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $startup 'RescueFlow Laptop Launcher.lnk'))
$shortcut.TargetPath = $pythonw
$shortcut.Arguments = ('"' + (Join-Path $root 'rescueflow_laptop_agent.py') + '"')
$shortcut.WorkingDirectory = $root
$shortcut.Description = 'RescueFlow local ESP32 controller remote launch agent'
$shortcut.Save()
Write-Host 'Enabled RescueFlow launcher when you sign in. Disconnect/reconnect to ESP32 Wi-Fi normally.'
