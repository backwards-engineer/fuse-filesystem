$ErrorActionPreference = 'Stop'
$runtime = Join-Path (Split-Path $PSScriptRoot -Parent) '.runtime'
foreach ($name in @('jenkins', 'gitea')) {
    $pidFile = "$runtime/$name.pid"
    if (!(Test-Path $pidFile)) { continue }
    $process = Get-Process -Id (Get-Content $pidFile) -ErrorAction SilentlyContinue
    $expected = if ($name -eq 'gitea') { "$runtime/gitea.exe" } else { (Get-ChildItem "$runtime/java/*/bin/java.exe")[0].FullName }
    if ($process -and $process.Path -eq $expected) { Stop-Process -Id $process.Id }
    Remove-Item -LiteralPath $pidFile
}
