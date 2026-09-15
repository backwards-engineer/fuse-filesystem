$ErrorActionPreference = 'Stop'
wsl.exe -d Ubuntu-24.04 -u root -- bash -lc 'apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake ninja-build pkg-config libfuse3-dev fuse3 python3 git dpkg-dev'
if ($LASTEXITCODE -ne 0) { throw 'Linux dependency installation failed. Install WSL2 Ubuntu-24.04 first.' }
