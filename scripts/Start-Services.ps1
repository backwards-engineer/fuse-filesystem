$ErrorActionPreference = 'Stop'
$runtime = Join-Path (Split-Path $PSScriptRoot -Parent) '.runtime'
$access = Get-Content "$runtime/access.json" -Raw | ConvertFrom-Json
$gitBin = Join-Path (Split-Path (Split-Path (Get-Command git.exe).Source -Parent) -Parent) 'bin'
$env:PATH = "$gitBin;$env:PATH"
function Start-LocalService($Name, $Executable, $Arguments) {
    $Executable = [IO.Path]::GetFullPath($Executable)
    $pidFile = "$runtime/$Name.pid"
    if (Test-Path $pidFile) {
        $existing = Get-Process -Id (Get-Content $pidFile) -ErrorAction SilentlyContinue
        if ($existing -and $existing.Path -eq $Executable) { Write-Host "$Name already running"; return }
    }
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $runtime -WindowStyle Hidden -PassThru -RedirectStandardOutput "$runtime/logs/$Name.out.log" -RedirectStandardError "$runtime/logs/$Name.err.log"
    $process.Id | Set-Content $pidFile
    Write-Host "Started $Name (PID $($process.Id))"
}
Start-LocalService 'gitea' "$runtime/gitea.exe" "web --work-path `"$runtime/gitea`" --config `"$runtime/gitea/app.ini`""
$env:JENKINS_HOME = "$runtime/jenkins"
$env:LOCAL_ADMIN_USER = $access.username
$env:LOCAL_JENKINS_PASSWORD = $access.jenkinsPassword
$env:LOCAL_GITEA_PASSWORD = $access.giteaPassword
$java = (Get-ChildItem "$runtime/java/*/bin/java.exe")[0].FullName
try {
    Start-LocalService 'jenkins' $java "-Xms256m -Xmx1024m -Djenkins.install.runSetupWizard=false -jar `"$runtime/jenkins.war`" --httpListenAddress=127.0.0.1 --httpPort=8080"
} finally {
    Remove-Item Env:LOCAL_JENKINS_PASSWORD,Env:LOCAL_GITEA_PASSWORD -ErrorAction SilentlyContinue
}
