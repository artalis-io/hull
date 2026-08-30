<#
  installer_tests.ps1 - installer-specific tests for install.ps1, run AS a
  standard (non-admin) user under BOTH Windows PowerShell 5.1 and PowerShell 7.

  Dot-sources install.ps1 (which defines its functions without running main) and
  exercises the factored logic with LOCAL fixtures (no network) for the
  fail-closed paths, plus gated network end-to-end installs against the official
  release when -IncludeNetwork is passed. Read-only w.r.t. GitHub releases:
  downloads only, never publishes or mutates anything.

  Usage: installer_tests.ps1 -InstallPs1 <path> [-IncludeNetwork] [-Evidence <file>]
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InstallPs1,
    [switch]$IncludeNetwork,
    [string]$Evidence
)

$ErrorActionPreference = 'Stop'
$script:pass = 0; $script:fail = 0
function Note($m) { if ($Evidence) { Add-Content -Path $Evidence -Value $m }; Write-Host $m }
function Ok($m)   { $script:pass++; Note ("  ok   " + $m) }
function Bad($m)  { $script:fail++; Note ("  FAIL " + $m) }
function Check([bool]$cond, [string]$m) { if ($cond) { Ok $m } else { Bad $m } }
function Throws([scriptblock]$sb, [string]$m) {
    $threw = $false
    try { & $sb } catch { $threw = $true }
    Check $threw $m
}

Note ("## installer_tests under PowerShell $($PSVersionTable.PSVersion) (as $(whoami))")

# Dot-source: defines the Hull* functions; the guard suppresses main because
# InvocationName is '.'.
. $InstallPs1

# Run install.ps1 as a CHILD PROCESS of the current PowerShell host, so its
# `exit` cannot terminate this test script and its exit code is observable. The
# child inherits this process's environment (redirected HOME/TEMP, and, on the
# 5.1 leg, the corrected PSModulePath).
$script:psExe = (Get-Process -Id $PID).Path
function Invoke-Install {
    param([string[]]$IArgs, [string]$Log)
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $InstallPs1) + $IArgs
    if ($Log) { & $script:psExe @a *> $Log } else { & $script:psExe @a *> $null }
    return $LASTEXITCODE
}

$work = Join-Path $env:TEMP ("hull-itest-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work -Force | Out-Null

try {
    # --- version tag grammar --------------------------------------------------
    Note "## version tag grammar"
    foreach ($v in @('v0.14.0', 'v1.2.3', 'v0.14.0-rc1', 'v10.0.0-beta.2')) {
        Check (Test-HullVersionTag $v) "accepts $v"
    }
    foreach ($v in @('0.14.0', 'v0.14', 'latest', 'v0.14.0; rm', '../evil', 'v0.14.0/..', 'v0.14.0 x')) {
        Check (-not (Test-HullVersionTag $v)) "rejects '$v'"
    }

    # --- manifest hash selection (fail closed) --------------------------------
    Note "## manifest hash selection"
    $h1 = ('a' * 64); $h2 = ('b' * 64); $h3 = ('c' * 64)
    $man = Join-Path $work 'hull.sha256'
    # Format is <64 hex><two spaces><name>.
    Set-Content -LiteralPath $man -Value @(
        "$h1  hull-cosmo",
        "$h2  hull-linux-x86_64",
        "garbage line without a hash",
        "$h3  hull-cosmocc.tar"
    )
    Check ((Get-HullManifestHash $man 'hull-cosmo') -eq $h1) "selects the exact hull-cosmo entry"
    Check ((Get-HullManifestHash $man 'hull-linux-x86_64') -eq $h2) "selects a different exact entry"
    Throws { Get-HullManifestHash $man 'hull-darwin-arm64' } "throws on a missing entry"
    $dup = Join-Path $work 'dup.sha256'
    Set-Content -LiteralPath $dup -Value @("$h1  hull-cosmo", "$h2  hull-cosmo")
    Throws { Get-HullManifestHash $dup 'hull-cosmo' } "throws on a duplicate entry"

    # --- sha-256 + checksum (fail closed) -------------------------------------
    Note "## sha-256 + checksum"
    $blob = Join-Path $work 'blob'
    [System.IO.File]::WriteAllBytes($blob, [System.Text.Encoding]::ASCII.GetBytes('hello'))
    $known = '2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824'
    Check ((Get-HullFileSha256 $blob) -eq $known) "sha256('hello') is correct"
    Check ((Test-HullChecksum $blob $known) -eq $known) "checksum match returns the hash"
    Throws { Test-HullChecksum $blob ('d' * 64) } "throws on a checksum mismatch"

    # --- prefix resolution ----------------------------------------------------
    Note "## prefix resolution"
    Check ((Resolve-HullPrefix '') -like '*\Programs\Hull') "default prefix is under Programs\Hull"
    Check ((Resolve-HullPrefix 'C:\x y\Hull') -eq 'C:\x y\Hull') "-Prefix override with spaces is honored verbatim"

    # --- PATH component comparison (pure) -------------------------------------
    Note "## PATH component comparison"
    Check (Test-HullPathContains 'C:\a;C:\b\' 'C:\b') "matches ignoring a trailing separator"
    Check (Test-HullPathContains 'C:\A' 'c:\a') "matches case-insensitively"
    Check (-not (Test-HullPathContains 'C:\a;C:\b' 'C:\c')) "does not match an absent dir"

    # --- rollback: a failed install leaves the previous binary runnable -------
    Note "## atomic install rollback"
    $rdir = Join-Path $work 'roll'; New-Item -ItemType Directory -Path $rdir -Force | Out-Null
    $rdest = Join-Path $rdir 'hull.com'
    $rsrc = Join-Path $work 'newbin'
    [System.IO.File]::WriteAllBytes($rsrc, [System.Text.Encoding]::ASCII.GetBytes('NEWBINARY'))
    [System.IO.File]::WriteAllBytes($rdest, [System.Text.Encoding]::ASCII.GetBytes('PREVIOUS'))
    Throws { Install-HullAtomicMove $rsrc $rdest -SimulateFailure } "simulated failed install throws"
    Check (Test-Path -LiteralPath $rdest) "previous binary still present after rollback"
    Check (((Get-Content -LiteralPath $rdest -Raw).Trim()) -eq 'PREVIOUS') "previous binary content intact (runnable)"
    Check (-not (Test-Path -LiteralPath "$rdest.new")) "staged .new cleaned up on failure"
    Check (-not (Test-Path -LiteralPath "$rdest.bak")) "backup .bak consumed by rollback"
    # success path replaces atomically and leaves no residue
    Install-HullAtomicMove $rsrc $rdest
    Check (((Get-Content -LiteralPath $rdest -Raw).Trim()) -eq 'NEWBINARY') "successful install replaces the binary"
    Check (-not (Test-Path -LiteralPath "$rdest.new") -and -not (Test-Path -LiteralPath "$rdest.bak")) "no staging residue after success"

    # --- user PATH add/remove (idempotent, exact, non-corrupting) -------------
    Note "## user PATH add/remove (HKCU, snapshot/restore)"
    $snap = Get-HullUserPathRaw
    try {
        $ptest = Join-Path $work 'pathdir'
        Check (Add-HullToUserPath $ptest $false) "adds an absent dir (returns added)"
        Check (Test-HullPathContains (Get-HullUserPathRaw).Value $ptest) "dir is present after add"
        Check (-not (Add-HullToUserPath $ptest $false)) "second add is idempotent (no-op)"
        $occ = @(((Get-HullUserPathRaw).Value -split ';') | Where-Object { (ConvertTo-HullPathKey $_) -eq (ConvertTo-HullPathKey $ptest) }).Count
        Check ($occ -eq 1) "exactly one occurrence after repeated adds"
        # original snapshot entries are all still present (non-corrupting)
        $curKeys = ((Get-HullUserPathRaw).Value -split ';') | ForEach-Object { ConvertTo-HullPathKey $_ }
        $allKept = $true
        foreach ($c in ($snap.Value -split ';')) { if ($c -eq '') { continue }; if (($curKeys -notcontains (ConvertTo-HullPathKey $c))) { $allKept = $false } }
        Check $allKept "existing PATH entries preserved after add"
        Check (Remove-HullFromUserPath $ptest $false) "removes the managed dir"
        Check (-not (Test-HullPathContains (Get-HullUserPathRaw).Value $ptest)) "dir absent after remove"
        Check (-not (Remove-HullFromUserPath $ptest $false)) "second remove is idempotent (no-op)"
        Check ((Get-HullUserPathRaw).Kind -eq $snap.Kind) "registry value kind preserved"
    } finally {
        Set-HullUserPathRaw $snap.Value $snap.Kind  # restore exactly
    }

    # --- network end-to-end (gated) -------------------------------------------
    if ($IncludeNetwork) {
        Note "## network end-to-end (official release, read-only)"

        # dry-run writes nothing
        $dpre = Join-Path $work 'dry\Hull'
        [void](Invoke-Install @('-Version', 'v0.14.0', '-Prefix', $dpre, '-NoPath', '-DryRun') (Join-Path $work 'dry.log'))
        Check (-not (Test-Path -LiteralPath (Join-Path $dpre 'hull.com'))) "dry-run performed no install"

        # default (latest) install into a spaces path, no PATH change
        $spre = Join-Path $work 'with space\Hull'
        $rc = Invoke-Install @('-Prefix', $spre, '-NoPath') (Join-Path $work 'inst-latest.log')
        $sbin = Join-Path $spre 'hull.com'
        Check ($rc -eq 0 -and (Test-Path -LiteralPath $sbin)) "latest install created hull.com in a spaces path"
        $sv = (& $sbin version 2>$null | Select-Object -First 1)
        Check ($sv -match '[0-9]+\.[0-9]+\.[0-9]+') "installed hull runs and reports a version ($sv)"

        # same-version reinstall without -Force is a clean no-op (not an error)
        $rc = Invoke-Install @('-Prefix', $spre, '-NoPath') (Join-Path $work 'inst-again.log')
        Check ($rc -eq 0) "same-version reinstall without -Force did not error"

        # explicit older version, then a different version without -Force must fail,
        # and with -Force must succeed.
        $vpre = Join-Path $work 'ver\Hull'
        [void](Invoke-Install @('-Version', 'v0.13.0', '-Prefix', $vpre, '-NoPath') (Join-Path $work 'inst-013.log'))
        $vbin = Join-Path $vpre 'hull.com'
        Check ((& $vbin version 2>$null | Select-Object -First 1) -match '0\.13\.0') "explicit v0.13.0 install reports 0.13.0"
        $rc = Invoke-Install @('-Version', 'v0.14.0', '-Prefix', $vpre, '-NoPath') (Join-Path $work 'inst-014-noforce.log')
        Check ($rc -ne 0) "different-version install without -Force is refused"
        Check ((& $vbin version 2>$null | Select-Object -First 1) -match '0\.13\.0') "the refused install left v0.13.0 runnable"
        $rc = Invoke-Install @('-Version', 'v0.14.0', '-Prefix', $vpre, '-NoPath', '-Force') (Join-Path $work 'inst-014-force.log')
        Check (($rc -eq 0) -and ((& $vbin version 2>$null | Select-Object -First 1) -match '0\.14\.0')) "forced replacement installs 0.14.0"

        # uninstall removes the managed exe + empty dir, idempotently
        [void](Invoke-Install @('-Uninstall', '-Prefix', $vpre) (Join-Path $work 'uninstall.log'))
        Check (-not (Test-Path -LiteralPath $vbin)) "uninstall removed the managed hull.com"
        $rc = Invoke-Install @('-Uninstall', '-Prefix', $vpre) (Join-Path $work 'uninstall2.log')
        Check ($rc -eq 0) "uninstall is idempotent (clean no-op)"
    } else {
        Note "## network end-to-end SKIPPED (no -IncludeNetwork)"
    }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Note ("## RESULT: $($script:pass) passed, $($script:fail) failed")
if ($script:fail -ne 0) { exit 1 }
exit 0
