<#
  installer_tests.ps1 - installer-specific tests for install.ps1, run AS a
  standard (non-admin) user under BOTH Windows PowerShell 5.1 and PowerShell 7.

  Dot-sources install.ps1 (which defines its functions without running main) and
  exercises the factored logic with LOCAL fixtures (no network) for the
  fail-closed paths, plus gated network end-to-end installs against the official
  release when -IncludeNetwork is passed. Read-only w.r.t. GitHub releases.

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
function WriteBytes([string]$p, [string]$s) { [System.IO.File]::WriteAllBytes($p, [System.Text.Encoding]::ASCII.GetBytes($s)) }
function ReadTrim([string]$p) { return ((Get-Content -LiteralPath $p -Raw).Trim()) }

Note ("## installer_tests under PowerShell $($PSVersionTable.PSVersion) (as $(whoami))")

. $InstallPs1

# Run install.ps1 as a CHILD PROCESS of the current host so its `exit` cannot
# terminate this test and its exit code is observable.
$script:psExe = (Get-Process -Id $PID).Path
function Invoke-Install {
    param([string[]]$IArgs, [string]$Log)
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $InstallPs1) + $IArgs
    if ($Log) { & $script:psExe @a *> $Log } else { & $script:psExe @a *> $null }
    return $LASTEXITCODE
}

# A compiled fake hull.exe (System.Diagnostics.Process runs a real PE, not a
# .cmd) whose behavior is env-driven: HULL_FAKE_MODE = ok|bad|absent|hang and
# HULL_FAKE_VERSION.
$script:FakeSrc = @'
using System;
class Program {
  static int Main(string[] a) {
    string mode = Environment.GetEnvironmentVariable("HULL_FAKE_MODE"); if (mode == null) mode = "ok";
    string cmd = a.Length > 0 ? a[0] : "";
    if (cmd == "version") {
      string v = Environment.GetEnvironmentVariable("HULL_FAKE_VERSION"); if (v == null) v = "9.9.9";
      Console.WriteLine("hull " + v); return 0;
    }
    if (cmd == "help" || cmd == "--help") {
      Console.WriteLine("Usage: hull <command>");
      Console.WriteLine("  version");
      Console.WriteLine("  build");
      if (mode != "absent") Console.WriteLine("  verify-release");
      return 0;
    }
    if (cmd == "verify-release") {
      bool help = a.Length > 1 && a[1] == "--help";
      if (mode == "absent") { Console.Error.WriteLine("unknown command: verify-release"); return 1; }
      if (mode == "hang") { System.Threading.Thread.Sleep(60000); return 0; }
      if (help) { Console.WriteLine("Usage: hull verify-release <manifest> <signature>"); return 0; }
      if (mode == "bad") return 1;
      return 0;
    }
    return 1;
  }
}
'@

$work = Join-Path $env:TEMP ("hull-itest-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work -Force | Out-Null

try {
    # --- version tag grammar --------------------------------------------------
    Note "## version tag grammar"
    foreach ($v in @('v0.14.0', 'v1.2.3', 'v0.14.0-rc1', 'v10.0.0-beta.2')) { Check (Test-HullVersionTag $v) "accepts $v" }
    foreach ($v in @('0.14.0', 'v0.14', 'latest', 'v0.14.0; rm', '../evil', 'v0.14.0/..', 'v0.14.0 x')) { Check (-not (Test-HullVersionTag $v)) "rejects '$v'" }

    # --- manifest hash selection (fail closed) --------------------------------
    Note "## manifest hash selection"
    $h1 = ('a' * 64); $h2 = ('b' * 64); $h3 = ('c' * 64)
    $man = Join-Path $work 'hull.sha256'
    Set-Content -LiteralPath $man -Value @("$h1  hull-cosmo", "$h2  hull-linux-x86_64", "garbage line without a hash", "$h3  hull-cosmocc.tar")
    Check ((Get-HullManifestHash $man 'hull-cosmo') -eq $h1) "selects the exact hull-cosmo entry"
    Throws { Get-HullManifestHash $man 'hull-darwin-arm64' } "throws on a missing entry"
    $dup = Join-Path $work 'dup.sha256'; Set-Content -LiteralPath $dup -Value @("$h1  hull-cosmo", "$h2  hull-cosmo")
    Throws { Get-HullManifestHash $dup 'hull-cosmo' } "throws on a duplicate entry"

    # --- sha-256 + checksum ---------------------------------------------------
    Note "## sha-256 + checksum"
    $blob = Join-Path $work 'blob'; WriteBytes $blob 'hello'
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
    Check (Test-HullPathContains 'C:/a;C:/b' 'C:\b') "matches across / and \ separators"
    Check (Test-HullPathContains '"C:\a";C:\b' 'C:\a') "matches ignoring surrounding quotes"
    $env:HULL_ITEST_VAR = 'C:\itest\dir'
    Check (Test-HullPathContains '%HULL_ITEST_VAR%' 'C:\itest\dir') "matches an expanded %VAR% against its expansion"
    Remove-Item Env:\HULL_ITEST_VAR -ErrorAction SilentlyContinue

    # --- user PATH add + exact one-entry removal (HKCU, snapshot/restore) ------
    Note "## user PATH add + one-entry removal"
    $snap = Get-HullUserPathRaw
    try {
        $ptest = Join-Path $work 'pathdir'
        Check ((Add-HullToUserPath $ptest $false) -eq $ptest) "add returns the exact raw entry"
        Check (Test-HullPathContains (Get-HullUserPathRaw).Value $ptest) "dir is present after add"
        Check ($null -eq (Add-HullToUserPath $ptest $false)) "second add is idempotent (returns null)"
        # inject a user-owned EQUIVALENT (trailing slash); removal must drop ONLY one
        $cur = Get-HullUserPathRaw; Set-HullUserPathRaw ($cur.Value.TrimEnd(';') + ';' + $ptest + '\') $cur.Kind
        $before = @(((Get-HullUserPathRaw).Value -split ';') | Where-Object { (ConvertTo-HullPathKey $_) -eq (ConvertTo-HullPathKey $ptest) }).Count
        Check ($before -eq 2) "two equivalent entries present before removal"
        Check (Remove-HullPathEntry $ptest $false) "removes one entry"
        $after = @(((Get-HullUserPathRaw).Value -split ';') | Where-Object { (ConvertTo-HullPathKey $_) -eq (ConvertTo-HullPathKey $ptest) }).Count
        Check ($after -eq 1) "exactly one equivalent entry removed, the other preserved"
        [void](Remove-HullPathEntry ($ptest + '\') $false)
        Check ((Get-HullUserPathRaw).Kind -eq $snap.Kind) "registry value kind preserved"
    } finally { Set-HullUserPathRaw $snap.Value $snap.Kind }

    # --- install transaction (binary + PATH + marker, all-or-nothing) ---------
    Note "## install transaction rollback"
    $tdir = Join-Path $work 'txn'; New-Item -ItemType Directory -Path $tdir -Force | Out-Null
    $tdest = Join-Path $tdir 'hull.com'
    $tsrc = Join-Path $work 'txnsrc'; WriteBytes $tsrc 'NEWTXN'
    WriteBytes $tdest 'PREVTXN'
    # pre-existing predictable sidecars must never be clobbered
    WriteBytes "$tdest.new" 'KEEPNEW'; WriteBytes "$tdest.bak" 'KEEPBAK'

    Throws { Invoke-HullCommitInstall $tsrc $tdest $tdir '9.9.9' $true -SimulateSwapFailure } "swap-phase failure throws"
    Check ((ReadTrim $tdest) -eq 'PREVTXN') "swap-phase failure restores the previous binary"
    Check (-not (Test-Path (Get-HullMarkerPath $tdir))) "swap-phase failure writes no marker"
    Check ((ReadTrim "$tdest.new") -eq 'KEEPNEW' -and (ReadTrim "$tdest.bak") -eq 'KEEPBAK') "pre-existing .new/.bak sidecars untouched"

    $snap2 = Get-HullUserPathRaw
    try {
        Throws { Invoke-HullCommitInstall $tsrc $tdest $tdir '9.9.9' $false -FailMarker } "marker-phase failure throws"
        Check ((ReadTrim $tdest) -eq 'PREVTXN') "marker-phase failure restores the previous binary"
        Check (-not (Test-Path (Get-HullMarkerPath $tdir))) "marker-phase failure leaves no marker"
        Check (-not (Test-HullPathContains (Get-HullUserPathRaw).Value $tdir)) "marker-phase failure undoes the PATH add"
    } finally { Set-HullUserPathRaw $snap2.Value $snap2.Kind }

    $critTxn = $null
    try { Invoke-HullCommitInstall $tsrc $tdest $tdir '9.9.9' $true -FailMarker -SimulateUndoFailure } catch { $critTxn = $_.Exception.Message }
    Check ($critTxn -match 'CRITICAL') "restore failure during commit raises CRITICAL"
    Check ($critTxn -match '\.hull-backup-') "CRITICAL names the preserved backup"
    $bakTxn = @(Get-ChildItem $tdir -Filter '.hull-backup-*' -Force -ErrorAction SilentlyContinue)
    Check (($bakTxn.Count -ge 1) -and ((ReadTrim $bakTxn[0].FullName) -eq 'PREVTXN')) "previous binary preserved in the backup"
    $bakTxn | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
    if (-not (Test-Path $tdest)) { WriteBytes $tdest 'PREVTXN' }

    Invoke-HullCommitInstall $tsrc $tdest $tdir '9.9.9' $true
    Check ((ReadTrim $tdest) -eq 'NEWTXN') "successful commit installs the new binary"
    Check (Test-HullMarkerValid (Read-HullMarker $tdir) $tdir) "successful commit writes a valid marker"
    Check (@(Get-ChildItem $tdir -Filter '.hull-*' -Force -ErrorAction SilentlyContinue).Count -eq 0) "no staging/backup residue after commit"

    # --- verifier detection (tri-state, bounded) ------------------------------
    Note "## verifier detection (tri-state, bounded)"
    $fakeExe = Join-Path $work 'fakehull.exe'
    Add-Type -TypeDefinition $script:FakeSrc -OutputAssembly $fakeExe -OutputType ConsoleApplication
    $sigFile = Join-Path $work 'sig.bin'; WriteBytes $sigFile 'SIG'
    $missingSig = Join-Path $work 'nope.sig'

    Check ((Get-HullVerifierState (Join-Path $work 'nope.com')) -eq 'none') "absent existing hull -> none (bootstrap ok)"
    $env:HULL_FAKE_MODE = 'ok';     Check ((Get-HullVerifierState $fakeExe) -eq 'capable') "verify-release-capable hull -> capable"
    $env:HULL_FAKE_MODE = 'bad';    Check ((Get-HullVerifierState $fakeExe) -eq 'capable') "capability is independent of the verify verdict"
    $env:HULL_FAKE_MODE = 'absent'; Check ((Get-HullVerifierState $fakeExe) -eq 'absent') "hull genuinely lacking verify-release -> absent (bootstrap ok)"
    $env:HULL_FAKE_MODE = 'hang';   Check ((Get-HullVerifierState $fakeExe 2) -eq 'ambiguous') "hung/timed-out probe -> ambiguous (abort, NOT bootstrap)"

    Note "## signature assertion"
    $env:HULL_FAKE_MODE = 'ok'
    $vok = $true; try { Assert-HullSignature $fakeExe $man $sigFile } catch { $vok = $false }
    Check $vok "capable + valid signature proceeds"
    $env:HULL_FAKE_MODE = 'bad'
    Throws { Assert-HullSignature $fakeExe $man $sigFile } "capable + invalid signature aborts"
    $env:HULL_FAKE_MODE = 'ok'
    Throws { Assert-HullSignature $fakeExe $man $missingSig } "capable + missing/undownloaded signature aborts"

    Note "## bounded artifact version"
    $env:HULL_FAKE_MODE = 'ok'; $env:HULL_FAKE_VERSION = '1.2.3'
    Check ((Get-HullArtifactVersion $fakeExe) -eq '1.2.3') "parses the artifact version"
    Remove-Item Env:\HULL_FAKE_MODE -ErrorAction SilentlyContinue
    Remove-Item Env:\HULL_FAKE_VERSION -ErrorAction SilentlyContinue

    # --- marker schema + prefix validation ------------------------------------
    Note "## marker schema + prefix validation"
    $mdir = Join-Path $work 'marker'; New-Item -ItemType Directory -Path $mdir -Force | Out-Null
    Write-HullMarker $mdir '1.0.0' $true 'C:\x'
    Check (Test-HullMarkerValid (Read-HullMarker $mdir) $mdir) "a well-formed marker at its prefix is valid"
    Check (-not (Test-HullMarkerValid (Read-HullMarker $mdir) (Join-Path $work 'other'))) "marker rejected for a different prefix"
    Set-Content -LiteralPath (Get-HullMarkerPath $mdir) -Encoding ASCII -Value (@{ installer = 'someone-else'; schema = 1; prefix = $mdir; version = '1'; pathAdded = $false } | ConvertTo-Json)
    Check (-not (Test-HullMarkerValid (Read-HullMarker $mdir) $mdir)) "marker with a foreign installer id rejected"
    Set-Content -LiteralPath (Get-HullMarkerPath $mdir) -Encoding ASCII -Value (@{ installer = 'install.ps1'; prefix = $mdir } | ConvertTo-Json)
    Check (-not (Test-HullMarkerValid (Read-HullMarker $mdir) $mdir)) "marker missing required fields rejected"
    Set-Content -LiteralPath (Get-HullMarkerPath $mdir) -Encoding ASCII -Value (@{ installer = 'install.ps1'; schema = 999; prefix = $mdir; version = '1'; pathAdded = $false } | ConvertTo-Json)
    Check (-not (Test-HullMarkerValid (Read-HullMarker $mdir) $mdir)) "marker with an unknown schema rejected"

    # --- no-adoption + uninstall ownership gate -------------------------------
    Note "## no-adoption + uninstall ownership gate"
    $udir = Join-Path $work 'unmanaged'; New-Item -ItemType Directory -Path $udir -Force | Out-Null
    $ubin = Join-Path $udir 'hull.com'; WriteBytes $ubin 'NOTOURS'
    Check (-not (Test-HullManagedHere $udir)) "an unmarked prefix is not managed (no adoption)"
    Invoke-HullUninstall -Prefix $udir -DryRun:$false | Out-Null
    Check (Test-Path -LiteralPath $ubin) "uninstall refuses to remove an unmanaged hull.com"
    Write-HullMarker $udir '0.0.0' $false ''
    Check (Test-HullManagedHere $udir) "a valid marker marks the prefix managed"
    Invoke-HullUninstall -Prefix $udir -DryRun:$false | Out-Null
    Check (-not (Test-Path -LiteralPath $ubin)) "uninstall removes a managed hull.com (pathAdded=false leaves PATH alone)"

    # --- network end-to-end (gated) -------------------------------------------
    if ($IncludeNetwork) {
        Note "## network end-to-end (official release, read-only)"

        $dpre = Join-Path $work 'dry\Hull'
        $tempBefore = @(Get-ChildItem -LiteralPath $env:TEMP -Filter 'hull-install-*' -Directory -Force -ErrorAction SilentlyContinue).Count
        [void](Invoke-Install @('-Version', 'v0.14.0', '-Prefix', $dpre, '-NoPath', '-DryRun') (Join-Path $work 'dry.log'))
        $tempAfter = @(Get-ChildItem -LiteralPath $env:TEMP -Filter 'hull-install-*' -Directory -Force -ErrorAction SilentlyContinue).Count
        Check (-not (Test-Path -LiteralPath $dpre)) "dry-run did not create the prefix"
        Check ($tempAfter -le $tempBefore) "dry-run created no hull-install temp dir"

        $spre = Join-Path $work 'with space\Hull'
        $rc = Invoke-Install @('-Prefix', $spre, '-NoPath') (Join-Path $work 'inst-latest.log')
        $sbin = Join-Path $spre 'hull.com'
        Check ($rc -eq 0 -and (Test-Path -LiteralPath $sbin)) "latest install created hull.com in a spaces path"
        Check ((& $sbin version 2>$null | Select-Object -First 1) -match '[0-9]+\.[0-9]+\.[0-9]+') "installed hull runs and reports a version"
        Check (Test-HullMarkerValid (Read-HullMarker $spre) $spre) "install wrote a valid ownership marker"

        $rc = Invoke-Install @('-Prefix', $spre, '-NoPath') (Join-Path $work 'inst-again.log')
        Check ($rc -eq 0) "same-version reinstall without -Force did not error"

        $vpre = Join-Path $work 'ver\Hull'
        [void](Invoke-Install @('-Version', 'v0.13.0', '-Prefix', $vpre, '-NoPath') (Join-Path $work 'inst-013.log'))
        $vbin = Join-Path $vpre 'hull.com'
        Check ((& $vbin version 2>$null | Select-Object -First 1) -match '0\.13\.0') "explicit v0.13.0 install reports 0.13.0"
        $rc = Invoke-Install @('-Version', 'v0.14.0', '-Prefix', $vpre, '-NoPath') (Join-Path $work 'inst-014-noforce.log')
        Check ($rc -ne 0) "different-version install without -Force is refused"
        Check ((& $vbin version 2>$null | Select-Object -First 1) -match '0\.13\.0') "the refused install left v0.13.0 runnable"
        $rc = Invoke-Install @('-Version', 'v0.14.0', '-Prefix', $vpre, '-NoPath', '-Force') (Join-Path $work 'inst-014-force.log')
        Check (($rc -eq 0) -and ((& $vbin version 2>$null | Select-Object -First 1) -match '0\.14\.0')) "forced replacement (with a real signature check) installs 0.14.0"

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
