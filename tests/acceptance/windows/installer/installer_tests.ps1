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
function BytesEqual([byte[]]$a, [byte[]]$b) {
    if ($null -eq $a -or $null -eq $b) { return $false }
    if ($a.Length -ne $b.Length) { return $false }
    for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { return $false } }
    return $true
}

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

    # --- user PATH add (normalized dedup) + EXACT-raw removal ownership -------
    Note "## user PATH add + exact-raw removal"
    $snap = Get-HullUserPathRaw
    try {
        $ptest = Join-Path $work 'pathdir'
        Check ((Add-HullToUserPath $ptest $false) -eq $ptest) "add returns the exact raw entry"
        Check (Test-HullPathContains (Get-HullUserPathRaw).Value $ptest) "dir is present after add"
        Check ($null -eq (Add-HullToUserPath $ptest $false)) "second add is idempotent (normalized dedup returns null)"
        # The user manually removes the installer's raw entry and later adds an
        # EQUIVALENT differently-spelled entry (trailing slash). Uninstall's exact
        # removal must NOT delete the user's replacement.
        $cur = Get-HullUserPathRaw
        $withoutOurs = (($cur.Value -split ';') | Where-Object { $_ -ne '' -and -not [string]::Equals($_, $ptest, [System.StringComparison]::Ordinal) })
        $userSpelling = $ptest + '\'
        Set-HullUserPathRaw ([string]::Join(';', (@($withoutOurs) + @($userSpelling)))) $cur.Kind
        Check (-not (Remove-HullPathEntryExact $ptest $false)) "exact removal of the installer entry is a no-op when only a differently-spelled user entry exists"
        Check (((Get-HullUserPathRaw).Value -split ';') -ccontains $userSpelling) "the user's differently-spelled equivalent is preserved"
        # Re-add ours alongside the user's, then exact-remove ONLY ours.
        $cur = Get-HullUserPathRaw; Set-HullUserPathRaw ($cur.Value.TrimEnd(';') + ';' + $ptest) $cur.Kind
        Check (Remove-HullPathEntryExact $ptest $false) "exact removal removes the installer's own raw entry"
        $parts = (Get-HullUserPathRaw).Value -split ';'
        Check (($parts -ccontains $userSpelling) -and (-not ($parts -ccontains $ptest))) "only the installer's raw entry was removed; the user's stayed"
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
    Check (@(Get-ChildItem -LiteralPath $tdir -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '.hull-stage-*' -or $_.Name -like '.hull-backup-*' }).Count -eq 0) "no staging/backup residue after commit"

    # --- UPGRADE rollback preserves the EXACT prior marker + PATH -------------
    Note "## upgrade rollback preserves the exact prior marker + PATH"
    $updir = Join-Path $work 'upgrade'; New-Item -ItemType Directory -Path $updir -Force | Out-Null
    $updest = Join-Path $updir 'hull.com'; WriteBytes $updest 'OLDBIN'
    Write-HullMarker $updir '0.13.0' $true $updir     # a valid prior managed marker (pathAdded, pathEntry=$updir)
    $priorMarkerBytes = [System.IO.File]::ReadAllBytes((Get-HullMarkerPath $updir))
    $snapU = Get-HullUserPathRaw
    try {
        # a nontrivial prior PATH containing the managed entry + odd formatting
        Set-HullUserPathRaw ($snapU.Value.TrimEnd(';') + ';' + $updir + ';C:\Weird Dir\;%TEMP%') $snapU.Kind
        $priorPath = Get-HullUserPathRaw
        $upsrc = Join-Path $work 'upsrc'; WriteBytes $upsrc 'NEWBIN'
        Throws { Invoke-HullCommitInstall $upsrc $updest $updir '0.14.0' $false -FailMarker } "upgrade marker failure throws"
        Check ((ReadTrim $updest) -eq 'OLDBIN') "upgrade rollback restores the previous binary"
        Check (BytesEqual ([System.IO.File]::ReadAllBytes((Get-HullMarkerPath $updir))) $priorMarkerBytes) "upgrade rollback preserves the prior marker BYTE-FOR-BYTE"
        $nowPath = Get-HullUserPathRaw
        Check (($nowPath.Value -ceq $priorPath.Value) -and ($nowPath.Kind -eq $priorPath.Kind)) "upgrade rollback restores the exact prior PATH value + kind"
    } finally { Set-HullUserPathRaw $snapU.Value $snapU.Kind }

    # --- rollback proof: a REAL (locked) removal failure preserves the backup -
    Note "## rollback proof: real locked-binary removal failure"
    $ldir = Join-Path $work 'lock'; New-Item -ItemType Directory -Path $ldir -Force | Out-Null
    $ldest = Join-Path $ldir 'hull.com'; WriteBytes $ldest 'OLDLOCK'
    $lsrc = Join-Path $work 'locksrc'; WriteBytes $lsrc 'NEWLOCK'
    $lswap = Start-HullBinarySwap $lsrc $ldest       # $ldest now = NEWLOCK, backup = OLDLOCK
    $fsLock = [System.IO.File]::Open($ldest, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::None)
    try {
        $critL = $null
        try { Undo-HullBinarySwap $lswap } catch { $critL = $_.Exception.Message }
        Check ($critL -match 'CRITICAL') "a locked (undeletable) new binary makes rollback raise CRITICAL"
        Check (Test-Path -LiteralPath $lswap.Backup) "the backup is preserved when the new binary cannot be removed"
        Check ((ReadTrim $lswap.Backup) -eq 'OLDLOCK') "the preserved backup still holds the previous binary bytes"
    } finally { $fsLock.Close(); $fsLock.Dispose() }
    Remove-Item -LiteralPath $ldest -Force -ErrorAction SilentlyContinue
    if ($lswap.Backup) { Remove-Item -LiteralPath $lswap.Backup -Force -ErrorAction SilentlyContinue }

    # --- verifier detection (tri-state, bounded) ------------------------------
    Note "## verifier detection (tri-state, bounded)"
    # Compile a real fake hull.exe with the Framework C# compiler (Add-Type
    # -OutputType ConsoleApplication is Windows-PowerShell-5.1-only, so csc.exe is
    # used directly for parity with PowerShell 7). System.Diagnostics.Process
    # needs a real PE, not a .cmd shim.
    $fakeExe = Join-Path $work 'fakehull.exe'
    $csFile = Join-Path $work 'fakehull.cs'; Set-Content -LiteralPath $csFile -Value $script:FakeSrc -Encoding ASCII
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    if (-not (Test-Path -LiteralPath $csc)) { $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe' }
    Check (Test-Path -LiteralPath $csc) "found a C# compiler for the test fake"
    & $csc /nologo "/out:$fakeExe" /target:exe "$csFile" *> (Join-Path $work 'csc.log')
    Check (Test-Path -LiteralPath $fakeExe) "compiled the fake hull.exe"
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

    # --- marker schema + prefix + type validation (fail closed) ---------------
    Note "## marker schema + prefix + type validation"
    $mdir = Join-Path $work 'marker'; New-Item -ItemType Directory -Path $mdir -Force | Out-Null
    $mk = Get-HullMarkerPath $mdir
    Write-HullMarker $mdir '1.0.0' $true $mdir
    Check (Test-HullMarkerValid (Read-HullMarker $mdir) $mdir) "a well-formed marker at its prefix is valid"
    Check (-not (Test-HullMarkerValid (Read-HullMarker $mdir) (Join-Path $work 'other'))) "marker rejected for a different prefix"
    Set-Content -LiteralPath $mk -Encoding ASCII -Value (@{ installer = 'install.ps1'; schema = 1; prefix = $mdir; version = '1'; pathAdded = $true; pathEntry = 'C:\somewhere\else' } | ConvertTo-Json)
    Check (-not (Test-HullMarkerValid (Read-HullMarker $mdir) $mdir)) "pathAdded=true with a pathEntry not bound to the prefix rejected"
    Set-Content -LiteralPath $mk -Encoding ASCII -Value (@{ installer = 'someone-else'; schema = 1; prefix = $mdir; version = '1'; pathAdded = $false; pathEntry = '' } | ConvertTo-Json)
    Check (-not (Test-HullMarkerValid (Read-HullMarker $mdir) $mdir)) "foreign installer id rejected"
    Set-Content -LiteralPath $mk -Encoding ASCII -Value (@{ installer = 'install.ps1'; prefix = $mdir; version = '1'; pathAdded = $false; pathEntry = '' } | ConvertTo-Json)
    Check (-not (Test-HullMarkerValid (Read-HullMarker $mdir) $mdir)) "missing required field (schema) rejected"
    Set-Content -LiteralPath $mk -Encoding ASCII -Value (@{ installer = 'install.ps1'; schema = 1; prefix = $mdir; version = '1'; pathAdded = $false } | ConvertTo-Json)
    Check (-not (Test-HullMarkerValid (Read-HullMarker $mdir) $mdir)) "missing pathEntry field rejected"
    Set-Content -LiteralPath $mk -Encoding ASCII -Value (@{ installer = 'install.ps1'; schema = 999; prefix = $mdir; version = '1'; pathAdded = $false; pathEntry = '' } | ConvertTo-Json)
    Check (-not (Test-HullMarkerValid (Read-HullMarker $mdir) $mdir)) "unknown schema rejected"
    Set-Content -LiteralPath $mk -Encoding ASCII -Value (@{ installer = 'install.ps1'; schema = 'abc'; prefix = $mdir; version = '1'; pathAdded = $false; pathEntry = '' } | ConvertTo-Json)
    Check (-not (Test-HullMarkerValid (Read-HullMarker $mdir) $mdir)) "non-numeric schema rejected (fail closed, no conversion error escapes)"
    Set-Content -LiteralPath $mk -Encoding ASCII -Value (@{ installer = 'install.ps1'; schema = 1; prefix = $mdir; version = '1'; pathAdded = 'yes'; pathEntry = 'x' } | ConvertTo-Json)
    Check (-not (Test-HullMarkerValid (Read-HullMarker $mdir) $mdir)) "non-boolean pathAdded rejected"
    Set-Content -LiteralPath $mk -Encoding ASCII -Value (@{ installer = 'install.ps1'; schema = 1; prefix = $mdir; version = '1'; pathAdded = $true; pathEntry = '' } | ConvertTo-Json)
    Check (-not (Test-HullMarkerValid (Read-HullMarker $mdir) $mdir)) "pathAdded=true with an empty pathEntry rejected"

    # --- atomic marker replace: a blocked replace retains the OLD marker ------
    Note "## atomic marker replace failure retains the old marker"
    $amdir = Join-Path $work 'atomicmarker'; New-Item -ItemType Directory -Path $amdir -Force | Out-Null
    $amdest = Join-Path $amdir 'hull.com'; WriteBytes $amdest 'OLDAM'
    Write-HullMarker $amdir '0.13.0' $true $amdir
    $oldAmBytes = [System.IO.File]::ReadAllBytes((Get-HullMarkerPath $amdir))
    $amsrc = Join-Path $work 'amsrc'; WriteBytes $amsrc 'NEWAM'
    $fsAm = [System.IO.File]::Open((Get-HullMarkerPath $amdir), [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        Throws { Invoke-HullCommitInstall $amsrc $amdest $amdir '0.14.0' $true } "commit fails when the marker cannot be atomically replaced"
    } finally { $fsAm.Close(); $fsAm.Dispose() }
    Check ((ReadTrim $amdest) -eq 'OLDAM') "binary rolled back when marker replacement fails"
    Check (BytesEqual ([System.IO.File]::ReadAllBytes((Get-HullMarkerPath $amdir))) $oldAmBytes) "the OLD marker is retained (never deleted) when replacement fails"

    # --- same-version re-affirmation is transactional -------------------------
    Note "## same-version re-affirmation transaction"
    $rfdir = Join-Path $work 'reaffirm'; New-Item -ItemType Directory -Path $rfdir -Force | Out-Null
    WriteBytes (Join-Path $rfdir 'hull.com') 'RFBIN'
    Write-HullMarker $rfdir '1.0.0' $false ''
    $rfSnap = Get-HullUserPathRaw
    try {
        Throws { Invoke-HullReaffirm $rfdir '1.0.0' $false -FailMarker } "re-affirmation marker failure throws"
        Check (-not (Test-HullPathContains (Get-HullUserPathRaw).Value $rfdir)) "re-affirmation marker failure undid the PATH add"
    } finally { Set-HullUserPathRaw $rfSnap.Value $rfSnap.Kind }

    # re-affirmation rollback failure must fail loudly (combined CRITICAL)
    $rf2dir = Join-Path $work 'reaffirm2'; New-Item -ItemType Directory -Path $rf2dir -Force | Out-Null
    WriteBytes (Join-Path $rf2dir 'hull.com') 'RF2'
    Write-HullMarker $rf2dir '1.0.0' $false ''
    $rf2Snap = Get-HullUserPathRaw
    try {
        $critRf = $null
        try { Invoke-HullReaffirm $rf2dir '2.0.0' $false -FailMarker -SimulateRestoreFailure } catch { $critRf = $_.Exception.Message }
        Check ($critRf -match 'CRITICAL') "re-affirmation rollback failure surfaces a combined CRITICAL"
        Check (($critRf -match 'PATH:') -and ($critRf -match 'marker:')) "the combined CRITICAL identifies the unrestored PATH and marker state"
    } finally { Set-HullUserPathRaw $rf2Snap.Value $rf2Snap.Kind }

    # --- a critical binary rollback still restores PATH + marker ---------------
    Note "## critical binary rollback still restores metadata"
    $cbdir = Join-Path $work 'critbin'; New-Item -ItemType Directory -Path $cbdir -Force | Out-Null
    $cbdest = Join-Path $cbdir 'hull.com'; WriteBytes $cbdest 'OLDCB'
    Write-HullMarker $cbdir '0.13.0' $true $cbdir
    $cbMarker = [System.IO.File]::ReadAllBytes((Get-HullMarkerPath $cbdir))
    $cbSnap = Get-HullUserPathRaw
    try {
        Set-HullUserPathRaw ($cbSnap.Value.TrimEnd(';') + ';' + $cbdir + ';C:\Odd Path\') $cbSnap.Kind
        $cbPrior = Get-HullUserPathRaw
        $cbsrc = Join-Path $work 'cbsrc'; WriteBytes $cbsrc 'NEWCB'
        $critCB = $null
        try { Invoke-HullCommitInstall $cbsrc $cbdest $cbdir '0.14.0' $false -FailMarker -SimulateUndoFailure } catch { $critCB = $_.Exception.Message }
        Check ($critCB -match 'CRITICAL') "a failed binary rollback surfaces a combined CRITICAL"
        $cbNow = Get-HullUserPathRaw
        Check (($cbNow.Value -ceq $cbPrior.Value) -and ($cbNow.Kind -eq $cbPrior.Kind)) "PATH restored despite the critical binary rollback"
        Check (BytesEqual ([System.IO.File]::ReadAllBytes((Get-HullMarkerPath $cbdir))) $cbMarker) "marker restored despite the critical binary rollback"
        Get-ChildItem -LiteralPath $cbdir -Filter '.hull-backup-*' -Force -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
    } finally { Set-HullUserPathRaw $cbSnap.Value $cbSnap.Kind }

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
