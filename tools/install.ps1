<#
.SYNOPSIS
    Install or upgrade a released EdgeClaw binary on Windows.

.DESCRIPTION
    Resolves the newest release, downloads the archive for this machine, verifies the release
    signature and the archive checksum, and puts the binary in place.

    Nothing here is an optional check. The binary this script installs runs with the user's
    privileges and holds their conversations and API key, so an archive that cannot be
    verified is not installed, and there is deliberately no switch to skip it.

    No dependency beyond what Windows 10 already carries: Invoke-WebRequest for the download,
    tar for the archive, Get-FileHash for the checksum, and .NET's ECDsa for the signature.

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

# The release signing public key. Compiled into the binary as well and committed as
# tools/update_signing.pub.pem -- three copies of one key, kept in step by
# tools/embed_pubkey_install.py and tools/gen_pubkey_c.py.
#
# It lives inside this script because the script is fetched standalone (curl/iwr | iex or a
# one-off download), with no checkout to read a file from.
$SigningPublicKeyPem = @'
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEUpTD9xJjm+ZfICQraLvCruln6hbY
DU6ZgAlq/8hcrdOd/fF0L28GFF4rkCzwf3VkPUhu5ozsA7OzLw1mJvsptQ==
-----END PUBLIC KEY-----
'@

function Fail {
    param([string]$Message)
    Write-Error $Message -ErrorAction Continue
    exit 1
}

function Get-Sha256Hex {
    param([string]$Path)
    (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# The archive for this machine. Only the two Windows targets are published; there is no
# arm64 Windows build, and saying so here is better than requesting a 404.
function Resolve-Target {
    $arch = $env:PROCESSOR_ARCHITECTURE
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
Verify sha256sums.txt against the embedded public key, then compare the archive against the
checksum it lists.

The ORDER is the point. Verifying the checksum first would accept an attacker who changed
both the archive and the sums file; verifying the signature first means the checksum being
compared against is one we trust.

ECDSA P-256 with SHA-256, matching what the release workflow signs with (openssl dgst -sha256
-sign). The signature file is base64 of the ASN.1 DER signature.
#>
function Test-Archive {
    param(
        [string]$ArchivePath,
        [string]$AssetName,
        [string]$WorkDir,
        [string]$ReleaseTag
    )

    $sumsUrl = "$Dl/$ReleaseTag/sha256sums.txt"
    $sigUrl = "$Dl/$ReleaseTag/sha256sums.txt.sig"
    $sumsPath = Join-Path $WorkDir 'sha256sums.txt'
    $sigPath = Join-Path $WorkDir 'sha256sums.txt.sig'

    try {
        Invoke-WebRequest -Uri $sumsUrl -OutFile $sumsPath -UseBasicParsing
    }
    catch {
        Fail @"
could not download $sumsUrl
This release was published without checksums, so it cannot be verified and will not be
installed. Use a release that includes sha256sums.txt, or build from source.
"@
    }

    try {
        Invoke-WebRequest -Uri $sigUrl -OutFile $sigPath -UseBasicParsing
    }
    catch {
        Fail @"
could not download $sigUrl
The checksums are not signed, so they could have been changed together with the archive.
Refusing to install.
"@
    }

    $sigB64 = (Get-Content -Path $sigPath -Raw).Trim()
    try {
        $sig = [Convert]::FromBase64String($sigB64)
    }
    catch {
        Fail 'the signature file is not valid base64'
    }

    $sumsBytes = [IO.File]::ReadAllBytes($sumsPath)
    $ecdsa = [System.Security.Cryptography.ECDsa]::Create()
    try {
        $ecdsa.ImportFromPem($SigningPublicKeyPem)
        if (-not $ecdsa.VerifyData($sumsBytes, $sig, [System.Security.Cryptography.HashAlgorithmName]::SHA256)) {
            Fail @"
the release signature does not verify
Either this release was not signed with the EdgeClaw release key, or sha256sums.txt was
modified after signing. Neither is safe to install. Do not continue; report it at
https://github.com/$Repo/issues
"@
        }
    }
    finally {
        $ecdsa.Dispose()
    }

    # Only now is the sums file trustworthy enough to read a checksum from it.
    $want = $null
    foreach ($line in (Get-Content -Path $sumsPath)) {
        $fields = $line.Trim() -split '\s+', 2
        if ($fields.Count -eq 2 -and $fields[1] -eq $AssetName) { $want = $fields[0].ToLowerInvariant(); break }
    }
    if (-not $want) {
        Fail @"
$AssetName is not listed in sha256sums.txt
The release is signed but does not cover this platform's archive, so it cannot be verified.
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

    Write-Host 'Verified signature and checksum'
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
        Fail @"
download failed: $url
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
