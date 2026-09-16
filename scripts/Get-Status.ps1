param([string]$JobName = 'local-fusefs')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$access = Get-Content "$root/.runtime/access.json" -Raw | ConvertFrom-Json
function New-AuthHeader($Password) {
    $pair = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("$($access.username):$Password"))
    return @{ Authorization = "Basic $pair" }
}
$giteaHeaders = New-AuthHeader $access.giteaPassword
$repo = Invoke-RestMethod 'http://127.0.0.1:3000/api/v1/repos/chris/local-fusefs' -Headers $giteaHeaders
Write-Host "Gitea: $($repo.html_url), private=$($repo.private), branch=$($repo.default_branch)"
$jenkinsHeaders = New-AuthHeader $access.jenkinsPassword
$job = Invoke-RestMethod "http://127.0.0.1:8080/job/$([Uri]::EscapeDataString($JobName))/api/json?tree=name,inQueue,lastBuild[number,result,building,url],lastSuccessfulBuild[number,url]" -Headers $jenkinsHeaders
$job | ConvertTo-Json -Depth 4
