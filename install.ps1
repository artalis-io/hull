<#
.SYNOPSIS
    Hull installer for Windows (PowerShell). Installs, upgrades, verifies, and
    removes Hull for the current user - no admin rights or Developer Mode.

.DESCRIPTION
    Downloads the official Cosmopolitan APE (hull-cosmo) from the artalis-io/hull
    GitHub release, verifies its SHA-256 against the signed hull.sha256 manifest
    before installation, and installs it as %LOCALAPPDATA%\Programs\Hull\hull.com
    on the current user's PATH. See docs/windows_install_design.md for the trust
    model.

    Trust: a same-channel SHA-256 protects against corruption and a mismatched
    asset, not a compromised release channel. Hull's Ed25519 release signature is
    the channel-independent authenticity root; a freshly-downloaded Hull cannot
    authenticate itself. On an upgrade, if a pre-existing Hull can verify the
    release signature, its check runs before replacement and a FAILURE or an
    ambiguous/timed-out probe aborts the install (never a silent downgrade to
    checksum-only). A first install proceeds under bootstrap trust.

.EXAMPLE
    irm https://gethull.dev/install.ps1 | iex

.EXAMPLE
    Invoke-WebRequest https://gethull.dev/install.ps1 -OutFile install.ps1
    .\install.ps1 -Version v0.14.0

.EXAMPLE
    .\install.ps1 -Uninstall
#>
[CmdletBinding(DefaultParameterSetName = 'Install')]
param(
    [Parameter(ParameterSetName = 'Install')]
    [string]$Version = 'latest',

    [string]$Prefix,

    [Parameter(ParameterSetName = 'Install')]
    [switch]$Force,

    [switch]$DryRun,

    [Parameter(ParameterSetName = 'Install')]
    [switch]$NoPath,

    [Parameter(ParameterSetName = 'Uninstall')]
    [switch]$Uninstall
)

$script:HullRepo       = 'artalis-io/hull'
$script:HullAsset      = 'hull-cosmo'
$script:HullBinName    = 'hull.com'
$script:HullMarkerName = '.hull-install.json'
$script:HullMarkerSchema = 1
$script:VersionTagPattern = '^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$'

function Write-HullInfo([string]$m) { Write-Host "hull: $m" }
function Write-HullWarn([string]$m) { Write-Host "hull: warning: $m" -ForegroundColor Yellow }
function Write-HullErr ([string]$m) { Write-Host "hull: error: $m" -ForegroundColor Red }

function Set-HullTls {
    try {
        [Net.ServicePointManager]::SecurityProtocol =
            [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    } catch { }
}

# ── Bounded native execution (PS 5.1 + 7; no hangs) ──────────────────────────
function ConvertTo-HullArgString([string[]]$Arguments) {
    $parts = @()
    foreach ($a in $Arguments) {
        if ($a -match '[\s"]') { $parts += '"' + ($a -replace '"', '\"') + '"' } else { $parts += $a }
    }
    return ($parts -join ' ')
}

function Invoke-HullBounded([string]$Exe, [string[]]$Arguments, [int]$TimeoutSec = 20) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = ConvertTo-HullArgString $Arguments
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    try { [void]$p.Start() } catch { return @{ Code = $null; Out = ''; TimedOut = $true } }
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill() } catch { }
        return @{ Code = $null; Out = ''; TimedOut = $true }
    }
    $out = ''
    try { $out = ($outTask.Result + "`n" + $errTask.Result) } catch { }
    return @{ Code = $p.ExitCode; Out = $out; TimedOut = $false }
}

# ── Version + release resolution ─────────────────────────────────────────────
function Test-HullVersionTag([string]$Tag) { return ($Tag -match $script:VersionTagPattern) }

function Resolve-HullTag([string]$Requested) {
    if ($Requested -and $Requested -ne 'latest') {
        if (-not (Test-HullVersionTag $Requested)) { throw "invalid -Version '$Requested' (expected a release tag like v0.14.0)" }
        return $Requested
    }
    $api = "https://api.github.com/repos/$($script:HullRepo)/releases/latest"
    $resp = Invoke-RestMethod -Uri $api -Headers @{ 'Accept' = 'application/vnd.github+json'; 'User-Agent' = 'hull-install.ps1' } -UseBasicParsing
    if ($resp.draft -eq $true -or $resp.prerelease -eq $true) { throw "latest resolved to a draft/prerelease ($($resp.tag_name)); refusing" }
    $tag = $resp.tag_name
    if (-not $tag -or -not (Test-HullVersionTag $tag)) { throw "could not resolve a valid latest release tag (got '$tag')" }
    return $tag
}

function Get-HullAssetUrls([string]$Tag) {
    $base = "https://github.com/$($script:HullRepo)/releases/download/$Tag"
    return @{ Asset = "$base/$($script:HullAsset)"; Manifest = "$base/hull.sha256"; Sig = "$base/hull.sha256.sig" }
}

function Get-HullFile([string]$Url, [string]$OutFile) {
    Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing -Headers @{ 'User-Agent' = 'hull-install.ps1' }
}

# ── Checksum ─────────────────────────────────────────────────────────────────
function Get-HullFileSha256([string]$Path) {
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        return ([System.BitConverter]::ToString($sha.ComputeHash($fs)) -replace '-', '').ToLower()
    } finally { $fs.Dispose() }
}

function Test-HullFileMatches([string]$Path, [string]$Hash, [long]$Size) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    if ((Get-Item -LiteralPath $Path).Length -ne $Size) { return $false }
    return ((Get-HullFileSha256 $Path) -eq $Hash)
}

function Get-HullManifestHash([string]$ManifestPath, [string]$AssetName) {
    $matched = @()
    foreach ($ln in (Get-Content -LiteralPath $ManifestPath)) {
        if ($ln -match '^([0-9a-fA-F]{64})\s\s(.+)$') { if ($Matches[2] -ceq $AssetName) { $matched += $Matches[1].ToLower() } }
    }
    if ($matched.Count -eq 0) { throw "no checksum entry for '$AssetName' in hull.sha256" }
    if ($matched.Count -gt 1) { throw "duplicate checksum entries for '$AssetName' in hull.sha256" }
    return $matched[0]
}

function Test-HullChecksum([string]$Path, [string]$Expected) {
    $actual = Get-HullFileSha256 $Path
    if ($actual -ne $Expected) { throw "checksum mismatch: expected $Expected, got $actual" }
    return $actual
}

# ── Verifier detection (tri-state, bounded) + signature assertion ────────────
function Get-HullVerifierState([string]$ExistingHull, [int]$TimeoutSec = 20) {
    if (-not $ExistingHull -or -not (Test-Path -LiteralPath $ExistingHull)) { return 'none' }
    $a = Invoke-HullBounded $ExistingHull @('verify-release', '--help') $TimeoutSec
    if ($a.TimedOut) { return 'ambiguous' }
    if ($a.Code -eq 0 -and $a.Out -match 'verify-release') { return 'capable' }
    $b = Invoke-HullBounded $ExistingHull @('help') $TimeoutSec
    if ($b.TimedOut) { return 'ambiguous' }
    if ($b.Code -eq 0) {
        if ($b.Out -match 'verify-release') { return 'capable' }
        if ($b.Out -match '(?im)usage:' -or $b.Out -match '(?im)^\s*version\b') { return 'absent' }
    }
    return 'ambiguous'
}

function Assert-HullSignature([string]$ExistingHull, [string]$ManifestPath, [string]$SigPath) {
    $sigItem = Get-Item -LiteralPath $SigPath -ErrorAction SilentlyContinue
    if (-not $sigItem -or $sigItem.Length -eq 0) { throw "release signature (hull.sha256.sig) is missing or empty; aborting" }
    $r = Invoke-HullBounded $ExistingHull @('verify-release', $ManifestPath, $SigPath) 30
    if ($r.TimedOut) { throw "release signature verification timed out; aborting" }
    if ($r.Code -ne 0) { throw "release signature did not verify; aborting (output: $($r.Out.Trim()))" }
}

function Get-HullArtifactVersion([string]$ExePath, [int]$TimeoutSec = 20) {
    $r = Invoke-HullBounded $ExePath @('version') $TimeoutSec
    if ($r.TimedOut) { return $null }
    if ($r.Out -match '([0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?)') { return $Matches[1] }
    return $null
}

# ── User PATH (HKCU only; type-preserving; normalized dedup, exact removal) ───
function Get-HullUserPathRaw {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $false)
    if ($null -eq $key) { return @{ Value = ''; Kind = [Microsoft.Win32.RegistryValueKind]::ExpandString } }
    try {
        $val = $key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $kind = [Microsoft.Win32.RegistryValueKind]::ExpandString
        try { if ($null -ne $key.GetValue('Path')) { $kind = $key.GetValueKind('Path') } } catch { }
        return @{ Value = [string]$val; Kind = $kind }
    } finally { $key.Close() }
}

function ConvertTo-HullPathKey([string]$Component) {
    $c = $Component.Trim().Trim('"')
    $c = [System.Environment]::ExpandEnvironmentVariables($c)
    $c = $c -replace '/', '\'
    $c = $c.TrimEnd('\')
    return $c.ToLowerInvariant()
}

function Test-HullPathContains([string]$RawPath, [string]$Dir) {
    $want = ConvertTo-HullPathKey $Dir
    foreach ($c in ($RawPath -split ';')) {
        if ($c -eq '') { continue }
        if ((ConvertTo-HullPathKey $c) -eq $want) { return $true }
    }
    return $false
}

function Set-HullUserPathRaw([string]$Value, $Kind) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
    if ($null -eq $key) { $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Environment') }
    try { $key.SetValue('Path', $Value, $Kind) } finally { $key.Close() }
}

function Restore-HullUserPathExact($Snap) {
    # Restore the EXACT raw value + registry kind, and prove it took.
    Set-HullUserPathRaw $Snap.Value $Snap.Kind
    $now = Get-HullUserPathRaw
    if (($now.Value -cne $Snap.Value) -or ($now.Kind -ne $Snap.Kind)) {
        throw "CRITICAL: could not restore the user PATH to its prior value/kind during rollback"
    }
}

function Send-HullEnvBroadcast {
    if (-not ('HullNative.Win32' -as [type])) {
        Add-Type -Namespace HullNative -Name Win32 -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Auto)]
public static extern System.IntPtr SendMessageTimeout(System.IntPtr hWnd, uint Msg, System.UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out System.UIntPtr lpdwResult);
'@ -ErrorAction SilentlyContinue
    }
    try {
        $out = [System.UIntPtr]::Zero
        [void][HullNative.Win32]::SendMessageTimeout([System.IntPtr]0xffff, 0x1a, [System.UIntPtr]::Zero, 'Environment', 0x2, 5000, [ref]$out)
    } catch { }
}

function Add-HullToUserPath([string]$Dir, [bool]$DryRun) {
    # Normalized dedup PREVENTS adding an equivalent duplicate. Returns the exact
    # raw component added (recorded for ownership), or $null if nothing added.
    $cur = Get-HullUserPathRaw
    if (Test-HullPathContains $cur.Value $Dir) { Write-HullInfo "PATH already contains $Dir"; return $null }
    if ($DryRun) { Write-HullInfo "[dry-run] would add $Dir to user PATH"; return $null }
    $newValue = if ([string]::IsNullOrEmpty($cur.Value)) { $Dir } else { ($cur.Value.TrimEnd(';') + ';' + $Dir) }
    Set-HullUserPathRaw $newValue $cur.Kind
    Send-HullEnvBroadcast
    Write-HullInfo "added $Dir to user PATH"
    return $Dir
}

function Remove-HullPathEntryExact([string]$RawEntry, [bool]$DryRun) {
    # Ownership at removal is proven by EXACT raw equality (ordinal), so a
    # differently-spelled equivalent that the user added is never deleted. Removes
    # at most one occurrence.
    if ([string]::IsNullOrEmpty($RawEntry)) { return $false }
    $cur = Get-HullUserPathRaw
    $kept = New-Object System.Collections.Generic.List[string]
    $removed = $false
    foreach ($c in ($cur.Value -split ';')) {
        if ($c -eq '') { continue }
        if (-not $removed -and [string]::Equals($c, $RawEntry, [System.StringComparison]::Ordinal)) { $removed = $true; continue }
        $kept.Add($c)
    }
    if (-not $removed) { return $false }
    if ($DryRun) { Write-HullInfo "[dry-run] would remove one PATH entry: $RawEntry"; return $false }
    Set-HullUserPathRaw ([string]::Join(';', $kept)) $cur.Kind
    Send-HullEnvBroadcast
    Write-HullInfo "removed PATH entry: $RawEntry"
    return $true
}

# ── Prefix ───────────────────────────────────────────────────────────────────
function Resolve-HullPrefix([string]$Requested) {
    if ($Requested) { return $Requested }
    $localApp = $env:LOCALAPPDATA
    if (-not $localApp) { $localApp = Join-Path $env:USERPROFILE 'AppData\Local' }
    return (Join-Path $localApp 'Programs\Hull')
}

# ── Ownership marker (validated schema + prefix + recorded raw PATH entry) ────
function Get-HullMarkerPath([string]$PrefixDir) { return (Join-Path $PrefixDir $script:HullMarkerName) }

function Write-HullMarkerAtomic([string]$PrefixDir, [byte[]]$Bytes) {
    # Write a unique temp in the same directory, then rename it into place. Uses
    # remove-then-rename (robust across NTFS/AV; File.Replace can throw when a
    # scanner briefly holds the destination).
    New-Item -ItemType Directory -Path $PrefixDir -Force | Out-Null
    $final = Get-HullMarkerPath $PrefixDir
    $tmp = Join-Path $PrefixDir (".hull-marker-" + [guid]::NewGuid().ToString('N'))
    [System.IO.File]::WriteAllBytes($tmp, $Bytes)
    try {
        if (Test-Path -LiteralPath $final) { Remove-Item -LiteralPath $final -Force }
        [System.IO.File]::Move($tmp, $final)
    } catch { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue; throw }
}

function Write-HullMarker([string]$PrefixDir, [string]$Ver, [bool]$PathAdded, [string]$PathEntry) {
    $rec = [ordered]@{ installer = 'install.ps1'; schema = $script:HullMarkerSchema; prefix = $PrefixDir; version = $Ver; pathAdded = $PathAdded; pathEntry = $PathEntry }
    Write-HullMarkerAtomic $PrefixDir ([System.Text.Encoding]::ASCII.GetBytes(($rec | ConvertTo-Json)))
}

function Read-HullMarker([string]$PrefixDir) {
    $m = Get-HullMarkerPath $PrefixDir
    if (-not (Test-Path -LiteralPath $m)) { return $null }
    try { return (Get-Content -LiteralPath $m -Raw | ConvertFrom-Json) } catch { return $null }
}

function Restore-HullMarkerExact([string]$PrefixDir, [byte[]]$PrevBytes) {
    $final = Get-HullMarkerPath $PrefixDir
    if ($null -eq $PrevBytes) {
        if (Test-Path -LiteralPath $final) { Remove-Item -LiteralPath $final -Force }
        if (Test-Path -LiteralPath $final) { throw "CRITICAL: could not remove a stray marker during rollback" }
        return
    }
    # If the marker on disk already matches the prior bytes, no rewrite is needed.
    $cur = $null
    if (Test-Path -LiteralPath $final) { try { $cur = [System.IO.File]::ReadAllBytes($final) } catch { } }
    $same = ($null -ne $cur) -and ($cur.Length -eq $PrevBytes.Length)
    if ($same) { for ($i = 0; $i -lt $cur.Length; $i++) { if ($cur[$i] -ne $PrevBytes[$i]) { $same = $false; break } } }
    if (-not $same) { Write-HullMarkerAtomic $PrefixDir $PrevBytes }
}

function Test-HullMarkerValid($Rec, [string]$PrefixDir) {
    # Ours only if it parses, carries our identity + schema + ALL required fields
    # with the right types, its recorded prefix is equivalent, and (when
    # pathAdded) it records a nonempty raw entry. Fail closed on malformed values.
    if ($null -eq $Rec) { return $false }
    $names = @($Rec.PSObject.Properties.Name)
    foreach ($f in @('installer', 'schema', 'prefix', 'version', 'pathAdded', 'pathEntry')) { if ($names -notcontains $f) { return $false } }
    if ($Rec.installer -ne 'install.ps1') { return $false }
    $schemaOk = $false; try { $schemaOk = ([int]$Rec.schema -eq $script:HullMarkerSchema) } catch { $schemaOk = $false }
    if (-not $schemaOk) { return $false }
    if ($Rec.pathAdded -isnot [bool]) { return $false }
    if ([string]::IsNullOrEmpty([string]$Rec.prefix)) { return $false }
    if (($Rec.pathAdded -eq $true) -and [string]::IsNullOrEmpty([string]$Rec.pathEntry)) { return $false }
    if ((ConvertTo-HullPathKey ([string]$Rec.prefix)) -ne (ConvertTo-HullPathKey $PrefixDir)) { return $false }
    return $true
}

function Test-HullManagedHere([string]$PrefixDir) { return (Test-HullMarkerValid (Read-HullMarker $PrefixDir) $PrefixDir) }

function Set-HullOwnershipMarker([string]$PrefixDir, [string]$Ver, [string]$AddedEntry) {
    # pathEntry is sticky: keep the exact recorded installer-added entry across
    # re-installs so uninstall removes exactly what we added.
    $prev = Read-HullMarker $PrefixDir
    $pathEntry = $AddedEntry
    if (-not $pathEntry -and (Test-HullMarkerValid $prev $PrefixDir) -and $prev.pathAdded) { $pathEntry = [string]$prev.pathEntry }
    Write-HullMarker $PrefixDir $Ver ([bool]$pathEntry) $pathEntry
}

# ── Binary swap + transaction (binary + PATH + marker, all-or-nothing) ───────
function Restore-HullPrevBinary([string]$Dest, [string]$Backup, [string]$PrevHash, [long]$PrevSize, [switch]$SimulateRestoreFailure, [switch]$SimulateRemoveFailure) {
    # Remove whatever occupies $Dest (the new/staged binary). A failure here is
    # CRITICAL - NEVER SilentlyContinue, and NEVER assume "the file exists" means
    # "the old binary is restored".
    if (Test-Path -LiteralPath $Dest) {
        if ($SimulateRemoveFailure) { throw "CRITICAL: could not remove the new binary during rollback; your previous hull is preserved at $Backup" }
        try { Remove-Item -LiteralPath $Dest -Force } catch { throw "CRITICAL: could not remove the new binary during rollback ($($_.Exception.Message)); your previous hull is preserved at $Backup" }
    }
    if (-not $SimulateRestoreFailure) { try { Move-Item -LiteralPath $Backup -Destination $Dest -Force } catch { } }
    # PROVE the restored binary is byte-identical to the recorded previous one
    # BEFORE deleting the backup.
    if (-not (Test-HullFileMatches $Dest $PrevHash $PrevSize)) {
        throw "CRITICAL: install failed AND rollback could not restore the previous hull; it is preserved at $Backup"
    }
    Remove-Item -LiteralPath $Backup -Force -ErrorAction SilentlyContinue
}

function Start-HullBinarySwap([string]$Src, [string]$Dest, [switch]$SimulateFailure) {
    $dir = Split-Path -Parent $Dest
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $staged = Join-Path $dir (".hull-stage-" + [guid]::NewGuid().ToString('N'))
    $backup = Join-Path $dir (".hull-backup-" + [guid]::NewGuid().ToString('N'))
    if ((Test-Path -LiteralPath $staged) -or (Test-Path -LiteralPath $backup)) { throw "sidecar path collision under $dir" }
    Copy-Item -LiteralPath $Src -Destination $staged -Force
    $hadPrev = Test-Path -LiteralPath $Dest
    $prevHash = $null; $prevSize = [long]0
    if ($hadPrev) { $prevHash = Get-HullFileSha256 $Dest; $prevSize = (Get-Item -LiteralPath $Dest).Length }
    try {
        if ($hadPrev) { Move-Item -LiteralPath $Dest -Destination $backup -Force }
        if ($SimulateFailure) { throw "simulated install failure (test)" }
        Move-Item -LiteralPath $staged -Destination $Dest -Force
    } catch {
        $err = $_
        Remove-Item -LiteralPath $staged -Force -ErrorAction SilentlyContinue
        if ($hadPrev) { Restore-HullPrevBinary $Dest $backup $prevHash $prevSize }
        throw $err
    }
    return @{ Backup = $(if ($hadPrev) { $backup } else { $null }); HadPrev = $hadPrev; PrevHash = $prevHash; PrevSize = $prevSize; Dest = $Dest }
}

function Complete-HullBinarySwap($Swap) {
    if ($Swap.HadPrev -and $Swap.Backup -and (Test-Path -LiteralPath $Swap.Backup)) {
        Remove-Item -LiteralPath $Swap.Backup -Force -ErrorAction SilentlyContinue
    }
}

function Undo-HullBinarySwap($Swap, [switch]$SimulateRestoreFailure, [switch]$SimulateRemoveFailure) {
    if (-not $Swap.HadPrev) {
        if (Test-Path -LiteralPath $Swap.Dest) {
            try { Remove-Item -LiteralPath $Swap.Dest -Force } catch { throw "CRITICAL: could not remove the newly-installed hull during rollback: $($Swap.Dest)" }
        }
        return
    }
    Restore-HullPrevBinary $Swap.Dest $Swap.Backup $Swap.PrevHash $Swap.PrevSize -SimulateRestoreFailure:$SimulateRestoreFailure -SimulateRemoveFailure:$SimulateRemoveFailure
}

function Invoke-HullCommitInstall([string]$Src, [string]$Dest, [string]$PrefixDir, [string]$Ver, [bool]$NoPath,
    [switch]$SimulateSwapFailure, [switch]$SimulateUndoFailure, [switch]$SimulateUndoRemoveFailure, [switch]$FailPath, [switch]$FailMarker) {

    # Snapshot the EXACT prior marker bytes (or absence) and raw PATH value+kind
    # BEFORE any mutation, so a later-phase failure restores them exactly.
    $markerPath = Get-HullMarkerPath $PrefixDir
    $prevMarkerBytes = $null
    if (Test-Path -LiteralPath $markerPath) { $prevMarkerBytes = [System.IO.File]::ReadAllBytes($markerPath) }
    $pathSnap = Get-HullUserPathRaw

    $swap = Start-HullBinarySwap $Src $Dest -SimulateFailure:$SimulateSwapFailure
    try {
        $addedEntry = $null
        if (-not $NoPath) {
            if ($FailPath) { throw "simulated PATH failure (test)" }
            $addedEntry = Add-HullToUserPath $PrefixDir $false
        }
        if ($FailMarker) { throw "simulated marker write failure (test)" }
        Set-HullOwnershipMarker $PrefixDir $Ver $addedEntry
        Complete-HullBinarySwap $swap
    } catch {
        $err = $_
        # Restore the binary FIRST (the anchor: a runnable hull), then the PATH
        # and marker to their exact prior state.
        Undo-HullBinarySwap $swap -SimulateRestoreFailure:$SimulateUndoFailure -SimulateRemoveFailure:$SimulateUndoRemoveFailure
        Restore-HullUserPathExact $pathSnap
        Restore-HullMarkerExact $PrefixDir $prevMarkerBytes
        throw $err
    }
}

# ── Install / uninstall ──────────────────────────────────────────────────────
function Invoke-HullInstall {
    param([string]$Version, [string]$Prefix, [bool]$Force, [bool]$DryRun, [bool]$NoPath)
    $ErrorActionPreference = 'Stop'

    Set-HullTls
    $tag = Resolve-HullTag $Version
    $urls = Get-HullAssetUrls $tag
    $prefixDir = Resolve-HullPrefix $Prefix
    $dest = Join-Path $prefixDir $script:HullBinName
    $wantVer = $tag.TrimStart('v')

    Write-HullInfo "repo:    $($script:HullRepo)"
    Write-HullInfo "version: $tag"
    Write-HullInfo "prefix:  $prefixDir"

    if ($DryRun) {
        Write-HullWarn "dry-run: no files, downloads, or PATH changes will be made"
        Write-HullInfo "[dry-run] would download $($script:HullAsset) for $tag and verify its SHA-256"
        Write-HullInfo "[dry-run] would install to $dest"
        if (-not $NoPath) { Write-HullInfo "[dry-run] would ensure $prefixDir is on the user PATH" }
        return
    }

    $installedVer = Get-HullArtifactVersion $dest
    if ((Test-Path -LiteralPath $dest) -and -not $Force) {
        if ($installedVer -eq $wantVer) {
            if (Test-HullManagedHere $prefixDir) {
                $added = $null
                if (-not $NoPath) { $added = Add-HullToUserPath $prefixDir $false }
                Set-HullOwnershipMarker $prefixDir $installedVer $added
                Write-HullInfo "hull $installedVer already installed and managed at $dest"
            } else {
                Write-HullInfo "hull $installedVer is already present at $dest but was not installed by install.ps1; not adopting it. Use -Force to replace it with a managed install."
            }
            return
        }
        throw "a different hull ($installedVer) is already installed at $dest; use -Force to replace it"
    }

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("hull-install-" + [guid]::NewGuid())
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    try {
        $asset = Join-Path $tmp $script:HullBinName
        $man   = Join-Path $tmp 'hull.sha256'
        $sig   = Join-Path $tmp 'hull.sha256.sig'

        Write-HullInfo "downloading $($script:HullAsset)"
        Get-HullFile $urls.Asset $asset
        Get-HullFile $urls.Manifest $man
        if (-not (Test-Path -LiteralPath $asset) -or (Get-Item $asset).Length -eq 0) { throw "download produced an empty file; release may not exist at $tag" }

        $actual = Test-HullChecksum $asset (Get-HullManifestHash $man $script:HullAsset)
        Write-HullInfo "checksum verified ($actual)"

        if (Test-Path -LiteralPath $dest) {
            switch (Get-HullVerifierState $dest) {
                'capable' {
                    Get-HullFile $urls.Sig $sig
                    Assert-HullSignature $dest $man $sig
                    Write-HullInfo "existing hull verified the release signature"
                }
                'absent' { Write-HullWarn "existing hull lacks 'verify-release'; proceeding under bootstrap trust (GitHub HTTPS + matched SHA-256)" }
                default  { throw "cannot determine whether the existing hull can verify signatures (the probe was ambiguous or timed out); aborting rather than downgrading trust. Remove or replace it, or use -Force with a fresh -Prefix." }
            }
        } else {
            Write-HullWarn "first install: bootstrap trust (GitHub HTTPS + matched SHA-256). Verify further with 'hull verify-release' and Sigstore/Rekor."
        }

        $candVer = Get-HullArtifactVersion $asset
        if ($candVer -ne $wantVer) { throw "downloaded artifact reports version '$candVer', expected '$wantVer'; aborting without changing the existing installation" }
        Write-HullInfo "artifact version check: $candVer"

        Invoke-HullCommitInstall $asset $dest $prefixDir $wantVer $NoPath
        Write-HullInfo "installed hull: $dest"

        Write-Host ""
        Write-HullInfo "next steps:"
        Write-Host "  hull doctor         . check the environment"
        Write-Host "  hull verify-release . verify a release manifest signature"
        Write-Host "  hull update         . install future releases"
        Write-Host ""
        Write-Host "Windows note: a v0.13.0 hull cannot self-update to v0.14.0 (the fix ships"
        Write-Host "in v0.14.0). If you are upgrading from v0.13.0, this manual install is the"
        Write-Host "one-time replacement; 'hull update' works normally from v0.14.0 onward."
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-HullUninstall {
    param([string]$Prefix, [bool]$DryRun)
    $ErrorActionPreference = 'Stop'

    $prefixDir = Resolve-HullPrefix $Prefix
    $dest = Join-Path $prefixDir $script:HullBinName
    $rec = Read-HullMarker $prefixDir

    if (-not (Test-HullMarkerValid $rec $prefixDir)) {
        Write-HullInfo "no valid install.ps1 ownership record at $prefixDir; refusing to remove anything not installed by install.ps1"
        return
    }

    if (Test-Path -LiteralPath $dest) {
        if ($DryRun) { Write-HullInfo "[dry-run] would remove $dest" }
        else { Remove-Item -LiteralPath $dest -Force; Write-HullInfo "removed $dest" }
    } else { Write-HullInfo "no hull found at $dest" }

    if ($rec.pathAdded -and $rec.pathEntry) { [void](Remove-HullPathEntryExact ([string]$rec.pathEntry) $DryRun) }
    else { Write-HullInfo "user PATH not modified (installer did not add an entry)" }

    if (-not $DryRun) {
        Remove-Item -LiteralPath (Get-HullMarkerPath $prefixDir) -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $prefixDir) {
            $remaining = @(Get-ChildItem -LiteralPath $prefixDir -Force -ErrorAction SilentlyContinue)
            if ($remaining.Count -eq 0) { Remove-Item -LiteralPath $prefixDir -Force -ErrorAction SilentlyContinue; Write-HullInfo "removed empty $prefixDir" }
        }
    }

    Write-Host ""
    Write-HullInfo "Hull state under ~/.hull (tools, caches, app data) was retained."
    Write-Host "  To remove it manually: Remove-Item -Recurse -Force `"$env:USERPROFILE\.hull`""
}

# ── Entry ────────────────────────────────────────────────────────────────────
function Invoke-HullInstallerMain {
    Write-Host ""
    Write-Host "Hull installer (Windows)" -ForegroundColor Cyan
    Write-Host ""
    try {
        if ($Uninstall) { Invoke-HullUninstall -Prefix $Prefix -DryRun:$DryRun.IsPresent }
        else { Invoke-HullInstall -Version $Version -Prefix $Prefix -Force:$Force.IsPresent -DryRun:$DryRun.IsPresent -NoPath:$NoPath.IsPresent }
    } catch {
        Write-HullErr $_.Exception.Message
        exit 1
    }
}

# Dot-sourcing defines the functions WITHOUT running main (for the tests). Normal
# execution and `irm | iex` run main.
if ($MyInvocation.InvocationName -ne '.') { Invoke-HullInstallerMain }
