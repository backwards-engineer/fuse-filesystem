param(
    [string]$Branch = 'codex/fusefs-storage-features',
    [string]$JobName = 'local-fusefs-storage-features'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$access = Get-Content "$root/.runtime/access.json" -Raw | ConvertFrom-Json
$pair = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("$($access.username):$($access.jenkinsPassword)"))
$headers = @{ Authorization = "Basic $pair" }
$server = 'http://127.0.0.1:8080'
$crumb = Invoke-RestMethod "$server/crumbIssuer/api/json" -Headers $headers -SessionVariable session
$headers[$crumb.crumbRequestField] = $crumb.crumb
[xml]$configuration = (Invoke-WebRequest "$server/job/local-fusefs/config.xml" -Headers $headers -WebSession $session).Content
$branchNode = $configuration.SelectSingleNode('//scm/branches/hudson.plugins.git.BranchSpec/name')
if (!$branchNode) { throw 'The source job does not contain a Git branch specification.' }
$branchNode.InnerText = "*/$Branch"
$configuration.SelectSingleNode('/flow-definition/description').InnerText = "Feature branch $Branch. Compiles, runs storage and real FUSE tests in WSL2, checks sanitizers, and archives benchmarks and packages."
$jobUrl = "$server/job/$([Uri]::EscapeDataString($JobName))"
$existing = Invoke-WebRequest "$jobUrl/api/json" -Headers $headers -WebSession $session -SkipHttpErrorCheck
if ($existing.StatusCode -eq 404) {
    $null = Invoke-WebRequest "$server/createItem?name=$([Uri]::EscapeDataString($JobName))" -Method Post -Headers $headers -WebSession $session -ContentType 'application/xml' -Body $configuration.OuterXml
} elseif ($existing.StatusCode -eq 200) {
    $null = Invoke-WebRequest "$jobUrl/config.xml" -Method Post -Headers $headers -WebSession $session -ContentType 'application/xml' -Body $configuration.OuterXml
} else { throw "Cannot inspect Jenkins job: HTTP $($existing.StatusCode)" }
$null = Invoke-WebRequest "$jobUrl/build" -Method Post -Headers $headers -WebSession $session
Write-Host "Configured and queued $jobUrl/ for $Branch"
