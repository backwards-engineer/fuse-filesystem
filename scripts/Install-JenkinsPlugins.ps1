$ErrorActionPreference = 'Stop'
$runtime = Join-Path (Split-Path $PSScriptRoot -Parent) '.runtime'
$version = '2.15.0'
$url = "https://github.com/jenkinsci/plugin-installation-manager-tool/releases/download/$version/jenkins-plugin-manager-$version.jar"
$target = "$runtime/plugin-manager.jar"
$hashText = (Invoke-WebRequest "$url.sha256").Content
if ($hashText -is [byte[]]) { $hashText = [Text.Encoding]::UTF8.GetString($hashText) }
$expected = ($hashText.Trim() -split '\s+')[0]
if (!(Test-Path $target)) { Invoke-WebRequest $url -OutFile $target }
if ((Get-FileHash $target -Algorithm SHA256).Hash -ne $expected) { throw 'Plugin manager checksum mismatch' }
$java = (Get-ChildItem "$runtime/java/*/bin/java.exe")[0].FullName
& $java -jar $target --war "$runtime/jenkins.war" --plugin-download-directory "$runtime/jenkins/plugins" --plugins workflow-aggregator git junit credentials-binding timestamper
if ($LASTEXITCODE -ne 0) { throw 'Jenkins plugin installation failed' }
