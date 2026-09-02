# Relaunches the retail Ballance client with the freshly built BallanceMMO
# plugin and physics_RT.dll, wiping the previous logs, and enables the file
# command channel (BMMO_COMMAND_FILE) so scripts/client_ctl.py can drive it.
#
#   launch_client.ps1 -GameDir <Ballance install> [-BuildDir <client build tree>]
#                     [-CmdFile <command file>] [-NoCopy] [-WaitSeconds N]
#
# GameDir defaults to $env:BMMO_GAME_DIR; BuildDir to ../build-client-stock.
param(
    [string]$GameDir = $env:BMMO_GAME_DIR,
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build-client-stock'),
    [string]$CmdFile = (Join-Path $env:TEMP 'bmmo_command.txt'),
    [switch]$NoCopy,
    [int]$WaitSeconds = 14
)
if (-not $GameDir) { throw 'pass -GameDir or set BMMO_GAME_DIR' }
Get-Process Player -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
if (-not $NoCopy) {
    Copy-Item -LiteralPath (Join-Path $BuildDir 'BallanceMMOClient\BallanceMMOClient.bmodp') -Destination "$GameDir\ModLoader\Mods\BallanceMMOClient.bmodp" -Force
    Copy-Item -LiteralPath (Join-Path $BuildDir 'BuildingBlocks\physics_RT.dll') -Destination "$GameDir\BuildingBlocks\physics_RT.dll" -Force
}
Remove-Item "$GameDir\Bin\Player.log" -ErrorAction SilentlyContinue
Remove-Item "$GameDir\ModLoader\ModLoader.log" -ErrorAction SilentlyContinue
Remove-Item $CmdFile, "$CmdFile.out" -ErrorAction SilentlyContinue
$env:BMMO_COMMAND_FILE = $CmdFile
$env:BMMO_COMMAND_PIPE = ''
$physLog = Join-Path ([System.IO.Path]::GetDirectoryName($CmdFile)) 'physics_rt_client.log'
Remove-Item $physLog -ErrorAction SilentlyContinue
$env:BMMO_PHYSICS_STDOUT = $physLog
$p = Start-Process -FilePath "$GameDir\Bin\Player.exe" -WorkingDirectory "$GameDir\Bin" -PassThru
Remove-Item Env:BMMO_COMMAND_FILE
Remove-Item Env:BMMO_PHYSICS_STDOUT
"launched pid=$($p.Id) cmdfile=$CmdFile"
Start-Sleep -Seconds $WaitSeconds
if ($p.HasExited) { "PLAYER EXITED early code=$($p.ExitCode)" } else { "player pid=$($p.Id) ALIVE" }
