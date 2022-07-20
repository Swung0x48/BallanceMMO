function ArchiveLogs {
  New-Item -ItemType Directory -Force -Path logs > $null
  Compress-Archive -Path "$LogName" -DestinationPath "logs\$LogName.zip" -Force
  Remove-Item -Path "$LogName"
}

try {
  while ($true) {
    $LogName = Get-Date -UFormat "log_%Y%m%d%H%M.log"
    .\BallanceMMOServer.exe --log=$LogName $args
    if ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 1) {
      break
    }
    else {
      ArchiveLogs
    }
  }
}
finally {
  ArchiveLogs
}