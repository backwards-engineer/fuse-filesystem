$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$runtime = Join-Path $root '.runtime'
New-Item -ItemType Directory -Force $runtime | Out-Null
function Get-Verified($Url, $Path, $Hash) {
    if (!(Test-Path $Path) -or (Get-FileHash $Path -Algorithm SHA256).Hash -ne $Hash) {
        Invoke-WebRequest $Url -OutFile $Path
    }
    if ((Get-FileHash $Path -Algorithm SHA256).Hash -ne $Hash) { throw "Checksum mismatch: $Path" }
}
$jdk = (Invoke-RestMethod 'https://api.adoptium.net/v3/assets/latest/21/hotspot?architecture=x64&image_type=jdk&os=windows')[0]
Get-Verified $jdk.binary.package.link "$runtime/java.zip" $jdk.binary.package.checksum
if (!(Test-Path "$runtime/java")) { Expand-Archive "$runtime/java.zip" "$runtime/java" }
$giteaVersion = '1.27.3'
$giteaUrl = "https://dl.gitea.com/gitea/$giteaVersion/gitea-$giteaVersion-windows-4.0-amd64.exe"
$giteaHash = ((Invoke-WebRequest "$giteaUrl.sha256").Content.Trim() -split '\s+')[0]
Get-Verified $giteaUrl "$runtime/gitea.exe" $giteaHash
$jenkinsVersion = '2.568.3'
$jenkinsUrl = "https://get.jenkins.io/war-stable/$jenkinsVersion/jenkins.war"
$checksumResponse = (Invoke-WebRequest "$jenkinsUrl.sha256").Content
if ($checksumResponse -is [byte[]]) { $checksumResponse = [Text.Encoding]::UTF8.GetString($checksumResponse) }
$jenkinsHash = ($checksumResponse.Trim() -split '\s+')[0]
Get-Verified $jenkinsUrl "$runtime/jenkins.war" $jenkinsHash
@{ java = $jdk.version.semver; gitea = $giteaVersion; jenkins = $jenkinsVersion; javaSha256 = $jdk.binary.package.checksum; giteaSha256 = $giteaHash; jenkinsSha256 = $jenkinsHash } | ConvertTo-Json | Set-Content "$runtime/versions.json"
Write-Host 'Downloaded and SHA-256 verified Java, Gitea, and Jenkins.'
