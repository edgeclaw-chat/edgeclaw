<#
.SYNOPSIS
    Make `bazel build` behave the same in Windows Terminal and in Git Bash, on
    any Windows machine, without depending on WSL and without baking a single
    machine-specific path into the repository.

.DESCRIPTION
    Two things on Windows are environment-dependent, and this script fixes both
    for the current user (HKCU only). Neither fix contains a hardcoded location:

      1. WHICH SHELL. Several genrules on the default build path are bash
         commands (//:models_catalog_json, the embed/version generators), so
         Bazel picks a bash -- and on Windows it picks one from the environment.
         Windows Terminal ends up with C:\Windows\System32\bash.exe (WSL, which
         only works if that distro is installed); Git Bash ends up with Git's own
         bash. Fix: set BAZEL_SH to the bash of the git that is on PATH here. The
         value is discovered on this machine, never assumed.

         This used to matter more: the flex/bison generators were also on the
         default path, so a build needed both tools as well. Their output is
         committed now and those rules are tagged manual, so a build needs only
         the shell.

      2. FLEX / BISON, for regenerating the grammar only. Not a build
         prerequisite -- src/shell/edc_lex.c and edc_parse.tab.c are committed.
         They are needed when you edit edc_lex.l / edc_parse.y, and then the
         entry point is tools/regen_grammar.sh. If they are not on PATH yet, pass
         -ToolsDir <dir> and this script adds that one directory to the user
         PATH; the directory is your input, not a default.

    Git Bash also needs one more thing: its MSYS runtime rewrites arguments that
    look like paths, which turns single-component Bazel labels such as
    `//tests:all` into `/tests:all` and makes Bazel fail with "invalid package
    name". The script adds a scoped opt-out to ~/.bashrc.

    Idempotent. Re-run after installing/moving Git or flex/bison.

.PARAMETER ToolsDir
    Directory containing flex and bison, used ONLY if they are not already on
    PATH. Not needed for a plain build; see (2) above.

.PARAMETER BashPath
    Explicit bash.exe. Default: derived from the `git` on PATH.

.PARAMETER SkipBashrc
    Do not touch ~/.bashrc (then export MSYS2_ARG_CONV_EXCL='//' yourself).

.PARAMETER CheckOnly
    Report what would change, change nothing.

.EXAMPLE
    .\tools\windows\setup-build-env.ps1

.EXAMPLE
    .\tools\windows\setup-build-env.ps1 -ToolsDir 'C:\msys64\usr\bin'
#>
[CmdletBinding()]
param(
    [string] $ToolsDir = '',
    [string] $BashPath = '',
    [switch] $SkipBashrc,
    [switch] $CheckOnly
)

$ErrorActionPreference = 'Stop'
$changes = @()

function Find-Bash {
    if ($BashPath) {
        if (-not (Test-Path $BashPath)) { throw "bash.exe not found at '$BashPath' (-BashPath)." }
        return (Resolve-Path $BashPath).Path
    }
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) {
        $roots = @()
        # <Git>\cmd\git.exe -> <Git>
        $roots += Split-Path (Split-Path $git.Source -Parent) -Parent
        # <Git>\mingw64\libexec\git-core -> <Git>
        $execPath = (& git --exec-path) 2>$null
        if ($execPath) { $roots += Split-Path (Split-Path (Split-Path $execPath -Parent) -Parent) -Parent }
        foreach ($root in $roots) {
            $candidate = Join-Path $root 'bin\bash.exe'
            if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
        }
    }
    throw 'No git found on PATH, so there is no bash to point Bazel at. Install Git for Windows, or pass -BashPath <bash.exe>.'
}

function Get-EffectivePath([string] $extra) {
    $machine = [Environment]::GetEnvironmentVariable('PATH', 'Machine')
    $user    = [Environment]::GetEnvironmentVariable('PATH', 'User')
    $parts = @()
    foreach ($chunk in @($machine, $user)) {
        if ($chunk) { $parts += , @($chunk -split ';' | Where-Object { $_ -ne '' }) }
    }
    $flat = @()
    foreach ($p in $parts) { $flat += $p }
    if ($extra) { $flat = @($extra) + $flat }
    return ($flat -join ';')
}

# Ask the bash itself what it can see: that is exactly what the genrule will do.
# The probe goes through a temporary script file, because passing this command
# line as an argument survives neither WSL's launcher nor every PowerShell
# quoting rule. Output is prefixed so it can be parsed without guessing.
function Invoke-BashProbe([string] $bash, [string] $pathValue) {
    $env:PATH = $pathValue
    $probeFile = Join-Path ([System.IO.Path]::GetTempPath()) ("edc-probe-" + [guid]::NewGuid().ToString('N') + ".sh")
    $script = @'
uname -s
echo "FL=$(command -v flex)"
echo "BI=$(command -v bison)"
echo "BV=$(bison --version 2>/dev/null | head -n1)"
'@
    [System.IO.File]::WriteAllText($probeFile, $script.Replace("`r`n", "`n"))
    try {
        $out = @(& $bash $probeFile 2>&1 | ForEach-Object { "$_" })
    } finally {
        Remove-Item $probeFile -ErrorAction SilentlyContinue
    }
    $field = @{}
    foreach ($line in $out) {
        $s = $line.Trim()
        if ($s -match '^(FL|BI|BV)=(.*)$') { $field[$Matches[1]] = $Matches[2].Trim() }
        elseif ($s -match '^(MINGW|MSYS|CYGWIN)') { $field['KIND'] = 'windows-msys' }
        elseif ($s -match 'microsoft|Microsoft') { $field['KIND'] = 'wsl' }
        elseif ($s -match '^Linux') { $field['KIND'] = 'linux' }
    }
    $flex  = if ($field.ContainsKey('FL')) { $field['FL'] } else { '' }
    $bison = if ($field.ContainsKey('BI')) { $field['BI'] } else { '' }
    $ver   = if ($field.ContainsKey('BV')) { $field['BV'] } else { '' }
    $okVer = $false
    if ($ver -and $ver -match 'Bison\)?\s+([0-9]+)\.') { $okVer = ([int]$Matches[1] -ge 3) }
    return [pscustomobject]@{
        Kind  = if ($field.ContainsKey('KIND')) { $field['KIND'] } else { 'unknown' }
        Flex  = $flex
        Bison = $bison
        Version = $ver
        Ok    = [bool]($flex -and $bison -and $okVer)
        OkVersion = $okVer
    }
}

Write-Host '== 1/3  which bash Bazel should use (BAZEL_SH) =='
$bash = Find-Bash
Write-Host "   $bash"
# Which bash this is matters, and it is asked rather than guessed from the
# path: a WSL distro may well have flex and bison, but depending on it is
# exactly what this setup exists to avoid. One plain command, no quoting games,
# so it answers under WSL too (a temp script path would not resolve there).
$kernel = ((& $bash -c 'uname -sr' 2>$null) | Select-Object -First 1)
if ("$kernel" -match '(?i)microsoft') {
    throw "That bash is WSL ($bash, $kernel). The point of this setup is to not depend on WSL -- install Git for Windows, or pass -BashPath <Git>\bin\bash.exe."
}
$probe = Invoke-BashProbe $bash (Get-EffectivePath '')
if (-not $probe.Ok) {
    Write-Host '   flex/bison are not both visible to it yet (see step 2).'
}

Write-Host '== 2/3  flex / bison, found on PATH =='
if ($probe.Ok) {
    Write-Host "   already on PATH: $($probe.Flex), $($probe.Version)"
} elseif ($ToolsDir) {
    if (-not (Test-Path (Join-Path $ToolsDir 'flex'))) {
        throw "No 'flex' in '$ToolsDir' (-ToolsDir). Expected the directory that holds flex and bison."
    }
    $probe2 = Invoke-BashProbe $bash (Get-EffectivePath $ToolsDir)
    if (-not $probe2.Ok) {
        $why = if (-not $probe2.OkVersion -and $probe2.Version) { "the bison found is too old: $($probe2.Version); GNU Bison 3.0+ is required" } else { 'flex and bison are still not both reachable' }
        throw "Even with '$ToolsDir' on PATH, $why."
    }
    $userPath = [Environment]::GetEnvironmentVariable('PATH', 'User')
    $entries = @(@($userPath) -split ';' | Where-Object { $_ -ne '' })
    if ($entries -notcontains $ToolsDir) {
        if (-not $CheckOnly) {
            [Environment]::SetEnvironmentVariable('PATH', (@($ToolsDir) + $entries) -join ';', 'User')
        }
        $changes += "PATH (user): prepended '$ToolsDir'"
    }
    Write-Host "   added to the user PATH: $ToolsDir"
    Write-Host "   -> $($probe2.Flex), $($probe2.Version)"
} else {
    throw @"
flex and bison are host tools (//src/shell:BUILD) and neither is on PATH, so
Bazel cannot generate the lexer and the parser. They are looked up on PATH --
the same contract the Linux and macOS builds use -- so install them somewhere
PATH reaches, or say where they are:

    .\tools\windows\setup-build-env.ps1 -ToolsDir <dir-with-flex-and-bison>

Windows has no system flex/bison. Get them from MSYS2 (`pacman -S flex m4 bison`)
and either put that prefix's bin on PATH, or point -ToolsDir at it.
GNU Bison 3.0+ is required (the grammar uses %define api.value.type / parse.error
and the genrule passes -Werror=conflicts-*).
"@
}

$current = [Environment]::GetEnvironmentVariable('BAZEL_SH', 'User')
if ($current -ne $bash) {
    if (-not $CheckOnly) { [Environment]::SetEnvironmentVariable('BAZEL_SH', $bash, 'User') }
    $changes += "BAZEL_SH: '$current' -> '$bash'"
}

Write-Host '== 3/3  Git Bash: keep MSYS from rewriting //labels =='
if ($SkipBashrc) {
    Write-Host "   skipped (-SkipBashrc); export MSYS2_ARG_CONV_EXCL='//' yourself"
} else {
    $rc = Join-Path $env:USERPROFILE '.bashrc'
    $has = (Test-Path $rc) -and ((@(Select-String -Path $rc -Pattern 'MSYS2_ARG_CONV_EXCL' -ErrorAction SilentlyContinue)).Count -gt 0)
    if ($has) {
        Write-Host "   already in $rc"
    } else {
        if (-not $CheckOnly) {
            Add-Content -Path $rc -Value "export MSYS2_ARG_CONV_EXCL='//'   # bazel labels start with //; keep MSYS from rewriting them" -Encoding ASCII
        }
        $changes += "~/.bashrc: added MSYS2_ARG_CONV_EXCL='//'"
        Write-Host "   $rc"
    }
}

Write-Host ''
if ($changes.Count -eq 0) {
    Write-Host 'Nothing to change: this machine is already set up.'
} elseif ($CheckOnly) {
    Write-Host 'Would change (user scope), nothing written (-CheckOnly):'
    $changes | ForEach-Object { Write-Host "  - $_" }
} else {
    Write-Host 'Changed (user scope):'
    $changes | ForEach-Object { Write-Host "  - $_" }
    Write-Host ''
    Write-Host 'Open a NEW terminal, then:'
    Write-Host '    bazel shutdown      # a running server keeps the environment it started with'
    Write-Host '    bazel build //src/tui:edgeclaw'
}
Write-Host ''
Write-Host 'To undo: [Environment]::SetEnvironmentVariable(''BAZEL_SH'', $null, ''User''),'
Write-Host 'drop the directory from the user PATH, and remove the line from ~/.bashrc.'
