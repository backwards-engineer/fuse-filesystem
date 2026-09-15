# Local FUSE filesystem and CI/CD

A Linux FUSE 3 starter filesystem, a private Gitea Git server, and Jenkins running on this Windows PC. Linux builds run as `chris` in WSL2 Ubuntu 24.04.

## Local services

| Service | Address |
| --- | --- |
| Git project | http://127.0.0.1:3000/chris/local-fusefs |
| Jenkins pipeline | http://127.0.0.1:8080/job/local-fusefs/ |

Both services listen only on `127.0.0.1`. Sign in as `chris`. Each service has its own generated password in `.runtime/access.json`, protected by a Windows file ACL and excluded from Git. These are separate from your Linux login. Gitea registrations and anonymous repository access are disabled; Jenkins requires sign-in.

The working repository is this directory. Gitea stores its bare repository and SQLite database in `.runtime/gitea`. Jenkins stores jobs, build logs, credentials, and archived packages in `.runtime/jenkins`. Keep `.runtime` when updating source code.

## Daily use

In PowerShell 7, from this directory:

```powershell
./scripts/Start-Services.ps1
./scripts/Get-Status.ps1
# Edit source, then:
git add src tests
git commit -m "Describe the change"
git push
```

Git may ask for the Gitea username/password on the first manual push. Jenkins polls `main` every minute and builds when it changes. You can also click **Build Now**. The pipeline checks out the private repository with Jenkins-managed credentials, compiles with warnings treated as errors, runs core tests and actual mount tests, creates `.deb` and `.tar.gz` packages with SHA-256 checksums, archives them, then deploys the successful binary to WSL staging:

```text
/home/chris/.local/opt/local-fusefs/releases/<git-commit>/local-fusefs
/home/chris/.local/opt/local-fusefs/current/local-fusefs
```

The `current` symlink selects the deployed revision. Staging does not mount the filesystem or start a permanent daemon. Previous deployed revisions remain available for manual rollback. Jenkins retains 20 build records and packages for the last 10 builds. Only trusted local code should run here: jobs use your Windows/WSL account.

Stop the services when builds are idle:

```powershell
./scripts/Stop-Services.ps1
```

These are background user processes, not Windows services. Run `Start-Services.ps1` after Windows restarts. Service output is in `.runtime/logs`. Stop both services before backing up `.runtime` together with the source repository; the backup contains credentials.

## Filesystem behavior

The filesystem is a read-only starter with a root directory and one file, `hello.txt`, containing `Hello from local-fusefs!` and a newline. It supports attributes, directory listing, opening and offset reads. It reports missing paths and rejects writes. Writable storage, persistence, and additional files are future project work.

Build and test in Ubuntu:

```bash
cd /mnt/c/Users/chris/Documents/ChatGPT/build-system
bash ci/build.sh
mkdir -p "$HOME/mnt/local-fusefs"
./build/local-fusefs -f -o ro "$HOME/mnt/local-fusefs"
```

In another Ubuntu terminal:

```bash
cat "$HOME/mnt/local-fusefs/hello.txt"
fusermount3 -u "$HOME/mnt/local-fusefs"
```

Compiler output lives under `~/.cache/local-fusefs` on the native Linux filesystem. The script copies the executable, test report, and packages back into this workspace's ignored `build/` and `artifacts/` folders.

Core tests cover path handling, bounded reads, negative and very large offsets, and EOF. Mount tests exercise directory listing, contents, seek, metadata, missing files, and write rejection. Missing `/dev/fuse` fails the build rather than skipping mount tests.

## Recreate the setup

Requires Git for Windows, PowerShell 7, and WSL2 Ubuntu 24.04 with user `chris`. Install WSL from an Administrator terminal with `wsl --install -d Ubuntu-24.04` if needed. Then:

```powershell
./scripts/Download-Tools.ps1
./scripts/Install-LinuxPrerequisites.ps1
./scripts/Install-JenkinsPlugins.ps1
./scripts/Initialize-Services.ps1
./scripts/Start-Services.ps1
# Wait for both login pages. The local repository must have a committed main branch.
./scripts/Initialize-Repository.ps1
```

Downloads use official release sites and verify SHA-256 checksums. Gitea and Jenkins versions are pinned in `Download-Tools.ps1`; Temurin Java 21 and compatible Jenkins plugins resolve from their official release metadata at installation time. Exact Java/core versions and binary checksums are recorded in `.runtime/versions.json`. The Jenkins bootstrap runs once, preserving later UI changes. This setup uses fixed local ports and assumes they are available.

Upstream documentation: [Gitea binary installation](https://docs.gitea.com/installation/install-from-binary/), [Jenkins WAR installation](https://www.jenkins.io/doc/book/installing/war-file/), [libfuse](https://github.com/libfuse/libfuse).
