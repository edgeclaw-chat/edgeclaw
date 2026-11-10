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

# The release signing public key, as the raw uncompressed P-256 point (0x04 || X || Y), hex.
#
# This form rather than PEM on purpose: "powershell" on a stock Windows 10/11 is Windows
# PowerShell 5.1, which runs on .NET Framework -- and .NET Framework has no
# ECDsa.ImportFromPem(). ECParameters/ImportParameters does exist there, and it takes the
# point. The same 65 bytes are compiled into the binary (src/common/edc_update_pubkey.c), so
# the two verifiers agree by construction rather than by both parsing ASN.1.
#
# The key is here rather than read from a file because this script is fetched standalone,
# with no checkout beside it.
$SigningPublicKeyHex = @'
045294c3f712639be65f20242b68bbc2aee967ea16d80d4e9980096affc85cadd39dfdf1742f6f06145e2b902cf07f75643d486ee68cec03b3b32f0d6626fb29b5
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

    # Build the P-256 public key from the raw point. ECParameters/ImportParameters works on
    # .NET Framework 4.6.2+ (Windows PowerShell 5.1) as well as .NET 5+ (pwsh 7), which is why
    # the key is stored as a point rather than as PEM.
    #
    # Converted byte by byte rather than with a cast or a regex split: `[byte[]] @('0x04', ...)`
    # does not parse a hex prefix, and the versions that tolerate it differ between Windows
    # PowerShell and pwsh. ToByte(base 16) is the same everywhere.
    $hex = $SigningPublicKeyHex.Trim()
    if ($hex.Length -ne 130) {
        Fail "the embedded public key is malformed (expected 130 hex characters, found $($hex.Length))"
    }
    $point = New-Object byte[] 65
    for ($i = 0; $i -lt 65; $i++) {
        $point[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
    }
    if ($point[0] -ne 0x04) {
        Fail 'the embedded public key is malformed (not an uncompressed EC point)'
    }

    # Every line here is the conservative spelling of the same thing, because the two
    # PowerShells differ: CreateFromFriendlyName rather than the nested
    # ECCurve+NamedCurves type (which resolves inconsistently), New-Object rather than ::new(),
    # and explicit [byte[]] casts because the slice of a byte[] is an Object[] and the implicit
    # conversion is not guaranteed across runtimes.
    $ecParams = New-Object System.Security.Cryptography.ECParameters
    $ecParams.Curve = [System.Security.Cryptography.ECCurve]::CreateFromFriendlyName('nistP256')
    $q = New-Object System.Security.Cryptography.ECPoint
    $q.X = [byte[]]$point[1..32]
    $q.Y = [byte[]]$point[33..64]
    $ecParams.Q = $q
    $ecParams = [System.Security.Cryptography.ECParameters]::new()
    $ecParams.Curve = [System.Security.Cryptography.ECCurve+NamedCurves]::nistP256
    $q = [System.Security.Cryptography.ECPoint]::new()
    $q.X = $point[1..32]
    $q.Y = $point[33..64]
    $ecParams.Q = $q

    $ecdsa = [System.Security.Cryptography.ECDsa]::Create()
    try {
        $ecdsa.ImportParameters($ecParams)
        # The signature is the ASN.1 DER form openssl produces (`openssl dgst -sha256 -sign`).
        # This overload's default format is DER on both .NET Framework and .NET 5+; the
        # DSASignatureFormat-taking overload does not exist on .NET Framework, so it cannot be
        # spelled out here without dropping Windows PowerShell 5.1 support.
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
