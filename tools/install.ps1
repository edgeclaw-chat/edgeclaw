<#
.SYNOPSIS
    Install or upgrade a released EdgeClaw binary on Windows.

.DESCRIPTION
    Resolves the newest release, downloads the archive for this machine, checks it against
    the sha256 the release publishes, and puts the binary in place.

    An archive whose checksum does not match is not installed, and there is deliberately no
    switch to skip the check. What the check does and does not prove is written out in
    Test-Archive below.

    No dependency beyond what Windows 10 already carries: Invoke-WebRequest for the download,
    tar for the archive, and Get-FileHash for the checksum.

.PARAMETER Tag
    Install this exact release instead of resolving the newest one. Also what the in-app
    updater passes, so the version the user was told about is the version installed.

.PARAMETER Pre
    Allow a pre-release when resolving the newest release. Without it, a pre-release is
    never picked implicitly.

.PARAMETER Dir
    Install into this directory instead of %USERPROFILE%\.local\bin. The in-app updater
    passes the directory the running binary is in, so an upgrade replaces itself.

.PARAMETER DryRun
    Resolve and print the tag, target and URL, then stop. Fetches no archive.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File install.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File install.ps1 -Tag v1.0.1 -Dir C:\Users\me\bin
#>
[CmdletBinding()]
param(
    [string]$Tag,
    [switch]$Pre,
    [string]$Dir,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Repo = 'edgeclaw-chat/edgeclaw'
$Api = "https://api.github.com/repos/$Repo/releases"
$Dl = "https://github.com/$Repo/releases/download"

function Fail {
    param([string]$Message)
    Write-Error $Message -ErrorAction Continue
    exit 1
}

function Get-Sha256Hex {
    param([string]$Path)
    (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# The archive for this machine. Only an amd64 Windows build is published.
#
# The architecture comes from the environment, which is right on Windows and absent
# elsewhere. Say what was actually seen when it is not that build: an empty value in the
# message reads like a bug in this script rather than a missing variable.
function Resolve-Target {
    $arch = $env:PROCESSOR_ARCHITECTURE
    if ([string]::IsNullOrEmpty($arch)) {
        Fail 'PROCESSOR_ARCHITECTURE is not set, so the target cannot be determined; this installer runs on Windows'
    }
    if ($arch -ne 'AMD64') {
        Fail "no Windows build for ${arch} -- only amd64 is published"
    }
    'edgeclaw_amd64_windows'
}

# Releases, newest first, as "tag<TAB>stable|pre". Same shape the sh installer parses, so the
# two agree on what counts as a candidate.
function Get-Releases {
    $resp = Invoke-WebRequest -Uri "$Api?per_page=20" -UseBasicParsing -Headers @{
        'Accept'     = 'application/vnd.github+json'
        'User-Agent' = 'edgeclaw-installer'
    }
    $items = $resp.Content | ConvertFrom-Json
    foreach ($r in $items) {
        if ($r.draft) { continue }
        $kind = if ($r.prerelease) { 'pre' } else { 'stable' }
        "$($r.tag_name)`t$kind"
    }
}

function Resolve-Tag {
    if ($Tag) { return $Tag }

    $releases = @(Get-Releases)
    if ($releases.Count -eq 0) { Fail "no published releases found at $Api" }

    if ($Pre) {
        return ($releases[0] -split "`t")[0]
    }

    foreach ($line in $releases) {
        $parts = $line -split "`t"
        if ($parts[1] -eq 'stable') { return $parts[0] }
    }

    # Not a generic failure: this is the expected state before the first stable tag, so say
    # what to do instead of only reporting that nothing was found.
    $list = ($releases | ForEach-Object { '  ' + ($_ -replace "`t", ' ') }) -join "`n"
    Fail @"
every published release is a pre-release, so there is no stable one to pick.
Pass -Pre to install the newest release anyway, or -Tag <tag> to choose one:
$list
"@
}

<#
Compare the archive against the sha256 the release publishes next to it.

That catches a truncated body, a wrong-platform asset, a stale CDN copy, and an archive
swapped by anything that is not GitHub itself.

What it does NOT do is prove authorship. `sha256sums.txt` is served from the same place as
the archive, so whoever can replace one can replace the other and this check would then agree
with itself. The remaining boundary is TLS to GitHub plus GitHub's access control over the
repository and its releases -- the same boundary Homebrew formulae and crates.io rely on.
Signing the release would move that boundary to "whoever holds the key"; it was built,
weighed, and dropped as more key management than this project's threat model justifies. See
the trust-boundary section of docs/auto-update.zh.md.

A missing checksum is fatal rather than a warning: an archive nobody can check is the case
where the comparison silently does nothing.
#>
function Test-Archive {
    param(
        [string]$ArchivePath,
        [string]$AssetName,
        [string]$WorkDir,
        [string]$ReleaseTag
    )

    $sumsUrl = "$Dl/$ReleaseTag/sha256sums.txt"
    $sumsPath = Join-Path $WorkDir 'sha256sums.txt'

    try {
        Invoke-WebRequest -Uri $sumsUrl -OutFile $sumsPath -UseBasicParsing
    }
    catch {
        Fail @"
could not download $sumsUrl
This release was published without checksums, so the archive cannot be checked and will not
be installed. Use a release that includes sha256sums.txt, or build from source.
"@
    }

    $want = $null
    foreach ($line in (Get-Content -Path $sumsPath)) {
        $fields = $line.Trim() -split '\s+', 2
        if ($fields.Count -eq 2 -and $fields[1] -eq $AssetName) { $want = $fields[0].ToLowerInvariant(); break }
    }
    if (-not $want) {
        Fail @"
$AssetName is not listed in sha256sums.txt
The release does not cover this platform's archive, so it cannot be checked.
"@
    }

    $have = Get-Sha256Hex -Path $ArchivePath
    if ($have -ne $want) {
        Fail @"
the downloaded archive does not match its checksum
  expected $want
  got      $have
The download is corrupt or was replaced in transit. Delete it and run this again.
"@
    }

    Write-Host 'Verified checksum'
}

function Install-Binary {
    param(
        [string]$ArchivePath,
        [string]$Target,
        [string]$InstallDir
    )

    $work = Join-Path ([IO.Path]::GetTempPath()) ("edgeclaw-install-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $work | Out-Null
    try {
        # The archive holds one directory named after the target, with the binary and the
        # licences. Look the binary up instead of reconstructing the path: a reconstructed path
        # silently produced "no binary" on a build that had just linked.
        tar -xzf $ArchivePath -C $work
        if ($LASTEXITCODE -ne 0) { Fail 'could not extract the archive' }

        $binary = Get-ChildItem -Path $work -Recurse -Filter 'edgeclaw.exe' -File |
            Where-Object { $_.DirectoryName -notlike "$work" } |
            Select-Object -First 1
        if (-not $binary) { Fail 'the archive contains no edgeclaw.exe' }

        if (-not (Test-Path -Path $InstallDir)) {
            New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
        }

        $dest = Join-Path $InstallDir 'edgeclaw.exe'
        $staging = "$dest.tmp$PID"

        # Copy next to the destination and move into place, so a reader sees either the old
        # binary or the new one and never a half-written file. Move-Item within one directory
        # is a rename; the .tmp name carries the pid so two concurrent runs do not collide.
        Copy-Item -Path $binary.FullName -Destination $staging -Force
        Move-Item -Path $staging -Destination $dest -Force

        Write-Host "Installed $dest"
        return $dest
    }
    finally {
        Remove-Item -Path $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ------------------------------------------------------------------------------- main

$target = Resolve-Target
$releaseTag = Resolve-Tag
$asset = "${target}_$releaseTag.tar.gz"
$url = "$Dl/$releaseTag/$asset"

if (-not $Dir) {
    if (-not $env:USERPROFILE) {
        Fail 'USERPROFILE is not set, so there is no default install directory -- pass -Dir <path>'
    }
    $Dir = Join-Path $env:USERPROFILE '.local\bin'
}

Write-Host "Installing $releaseTag for $target"

if ($DryRun) {
    Write-Host "tag        $releaseTag"
    Write-Host "target     $target"
    Write-Host "asset      $asset"
    Write-Host "url        $url"
    Write-Host "installdir $Dir"
    exit 0
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("edgeclaw-dl-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $archivePath = Join-Path $work $asset
    Write-Host "Downloading $asset"
    try {
        Invoke-WebRequest -Uri $url -OutFile $archivePath -UseBasicParsing
    }
    catch {
        # The exception text is included: without it a failure here reads as "the URL must be
        # wrong", when the usual cause is that the connection never happened at all (a sandbox
        # that isolates loopback, a DNS failure, a proxy).
        Fail @"
download failed: $url
$($_.Exception.Message)
The tag exists but has no $target asset, or the network is unavailable. Check the assets at
https://github.com/$Repo/releases/tag/$releaseTag
"@
    }

    Test-Archive -ArchivePath $archivePath -AssetName $asset -WorkDir $work -ReleaseTag $releaseTag

    $dest = Install-Binary -ArchivePath $archivePath -Target $target -InstallDir $Dir

    $sep = [IO.Path]::PathSeparator
    $onPath = ($env:PATH -split [regex]::Escape($sep)) -contains $Dir
    if (-not $onPath) {
        Write-Host ''
        Write-Host "$Dir is not on your PATH. Add it, for example:"
        Write-Host "  [Environment]::SetEnvironmentVariable('PATH', `"$Dir;$env:PATH`", 'User')"
    }
}
finally {
    Remove-Item -Path $work -Recurse -Force -ErrorAction SilentlyContinue
}
