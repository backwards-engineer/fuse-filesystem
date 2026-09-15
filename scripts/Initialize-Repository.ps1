$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$access = Get-Content "$root/.runtime/access.json" -Raw | ConvertFrom-Json
$encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("$($access.username):$($access.giteaPassword)"))
$headers = @{ Authorization = "Basic $encoded" }
$uri = "http://127.0.0.1:3000/api/v1/repos/$($access.username)/local-fusefs"
$response = Invoke-WebRequest -Uri $uri -Headers $headers -SkipHttpErrorCheck
if ($response.StatusCode -eq 404) {
    $body = @{ name = 'local-fusefs'; description = 'Linux FUSE filesystem with local Jenkins CI/CD'; private = $true; auto_init = $false; default_branch = 'main' } | ConvertTo-Json
    $null = Invoke-RestMethod 'http://127.0.0.1:3000/api/v1/user/repos' -Method Post -Headers $headers -ContentType 'application/json' -Body $body
} elseif ($response.StatusCode -ne 200) { throw "Gitea API returned $($response.StatusCode)" }
Push-Location $root
try {
    $remote = 'http://127.0.0.1:3000/chris/local-fusefs.git'
    $existing = git remote get-url origin 2>$null
    if (!$existing) { git remote add origin $remote }
    elseif ($existing -ne $remote) { throw 'Existing origin differs; refusing to overwrite it.' }
    $env:GIT_CONFIG_COUNT = '1'
    $env:GIT_CONFIG_KEY_0 = 'http.http://127.0.0.1:3000/.extraHeader'
    $env:GIT_CONFIG_VALUE_0 = "Authorization: Basic $encoded"
    git push -u origin main
    if ($LASTEXITCODE -ne 0) { throw 'Initial push failed' }
} finally {
    Remove-Item Env:GIT_CONFIG_COUNT,Env:GIT_CONFIG_KEY_0,Env:GIT_CONFIG_VALUE_0 -ErrorAction SilentlyContinue
    Pop-Location
}
Write-Host 'Private project: http://127.0.0.1:3000/chris/local-fusefs'
