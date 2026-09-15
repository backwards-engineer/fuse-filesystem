$ErrorActionPreference = 'Stop'
if ([Environment]::UserName -like 'CodexSandbox*') { throw 'Run initialization as the Windows account that will run the services.' }
$root = Split-Path $PSScriptRoot -Parent
$runtime = Join-Path $root '.runtime'
New-Item -ItemType Directory -Force "$runtime/gitea", "$runtime/jenkins/init.groovy.d", "$runtime/logs" | Out-Null
if (!(Test-Path "$runtime/access.json")) {
    function New-Secret { return [Convert]::ToHexString([Security.Cryptography.RandomNumberGenerator]::GetBytes(24)).ToLowerInvariant() }
    @{ username = 'chris'; giteaPassword = (New-Secret); jenkinsPassword = (New-Secret) } | ConvertTo-Json | Set-Content "$runtime/access.json"
    $principal = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    & icacls "$runtime/access.json" /inheritance:r /grant:r "${principal}:(F)" 'SYSTEM:(F)' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Failed to protect credentials file' }
}
$access = Get-Content "$runtime/access.json" -Raw | ConvertFrom-Json
if (!(Test-Path "$runtime/gitea/app.ini")) {
    $data = "$runtime/gitea".Replace('\','/')
    @"
APP_NAME = Local Git
RUN_USER = $env:USERNAME
RUN_MODE = prod
[database]
DB_TYPE = sqlite3
PATH = $data/gitea.db
[repository]
ROOT = $data/repositories
DEFAULT_BRANCH = main
[server]
APP_DATA_PATH = $data/data
HTTP_ADDR = 127.0.0.1
HTTP_PORT = 3000
DOMAIN = 127.0.0.1
ROOT_URL = http://127.0.0.1:3000/
DISABLE_SSH = true
OFFLINE_MODE = true
[security]
INSTALL_LOCK = true
[service]
DISABLE_REGISTRATION = true
REQUIRE_SIGNIN_VIEW = true
[log]
MODE = console
LEVEL = Warn
"@ | Set-Content "$runtime/gitea/app.ini"
    & "$runtime/gitea.exe" --work-path "$runtime/gitea" --config "$runtime/gitea/app.ini" migrate
    if ($LASTEXITCODE -ne 0) { throw 'Gitea migration failed' }
}
$users = & "$runtime/gitea.exe" --work-path "$runtime/gitea" --config "$runtime/gitea/app.ini" admin user list
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect Gitea users' }
if (!(($users -join "`n") -match '(?m)^\d+\s+chris\s')) {
    & "$runtime/gitea.exe" --work-path "$runtime/gitea" --config "$runtime/gitea/app.ini" admin user create --username $access.username --password $access.giteaPassword --email 'chris@localhost.local' --admin --must-change-password=false
    if ($LASTEXITCODE -ne 0) { throw 'Gitea account creation failed' }
}
Copy-Item "$PSScriptRoot/jenkins-init.groovy" "$runtime/jenkins/init.groovy.d/01-local-setup.groovy" -Force
Write-Host 'Service configuration initialized. Credentials are in .runtime/access.json (excluded from Git).'
