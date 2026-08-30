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
    authenticate itself. On an upgrade, if a pre-existing Hull supports
    'verify-release', its signature check runs before replacement and a FAILURE
    aborts the install (never a silent downgrade to checksum-only). A first
    install proceeds under bootstrap trust (GitHub HTTPS + a matched SHA-256);
    verify further out-of-band with 'hull verify-release' and Sigstore/Rekor.

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

    # -Prefix and -DryRun compose with both install and uninstall.
    [string]$Prefix,

    [Parameter(ParameterSetName = 'Install')]
    [switch]$Force,

    [switch]$DryRun,

    [Parameter(ParameterSetName = 'Install')]
    [switch]$NoPath,

    [Parameter(ParameterSetName = 'Uninstall')]
    [switch]$Uninstall
)

# The upstream repository is FIXED. The public installer exposes no override.
$script:HullRepo   = 'artalis-io/hull'
$script:HullAsset  = 'hull-cosmo'
$script:HullBinName = 'hull.com'
$script:VersionTagPattern = '^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$'

# ── Output helpers (never log secrets, proxy creds, or temp URLs) ─────────────
function Write-HullInfo([string]$m) { Write-Host "hull: $m" }
function Write-HullWarn([string]$m) { Write-Host "hull: warning: $m" -ForegroundColor Yellow }
function Write-HullErr ([string]$m) { Write-Host "hull: error: $m" -ForegroundColor Red }

# ── TLS + defaults ───────────────────────────────────────────────────────────
function Set-HullTls {
    # Windows PowerShell 5.1 defaults to TLS 1.0/1.1; force >= 1.2. Add flags
    # rather than replacing so PowerShell 7 keeps its negotiated set.
    try {
        [Net.ServicePointManager]::SecurityProtocol =
            [Net.ServicePointManager]::SecurityProtocol -bor
            [Net.SecurityProtocolType]::Tls12
    } catch { }
}

# ── Version + release resolution ─────────────────────────────────────────────
function Test-HullVersionTag([string]$Tag) {
    return ($Tag -match $script:VersionTagPattern)
}

function Resolve-HullTag([string]$Requested) {
    if ($Requested -and $Requested -ne 'latest') {
        if (-not (Test-HullVersionTag $Requested)) {
            throw "invalid -Version '$Requested' (expected a release tag like v0.14.0)"
        }
        return $Requested
    }
    # /releases/latest already excludes drafts and prereleases.
    $api = "https://api.github.com/repos/$($script:HullRepo)/releases/latest"
    $headers = @{ 'Accept' = 'application/vnd.github+json'; 'User-Agent' = 'hull-install.ps1' }
    $resp = Invoke-RestMethod -Uri $api -Headers $headers -UseBasicParsing
    $tag = $resp.tag_name
    if ($resp.draft -eq $true -or $resp.prerelease -eq $true) {
        throw "latest release resolved to a draft/prerelease ($tag); refusing"
    }
    if (-not $tag -or -not (Test-HullVersionTag $tag)) {
        throw "could not resolve a valid latest release tag (got '$tag')"
    }
    return $tag
}

function Get-HullAssetUrls([string]$Tag) {
    $base = "https://github.com/$($script:HullRepo)/releases/download/$Tag"
    return @{
        Asset    = "$base/$($script:HullAsset)"
        Manifest = "$base/hull.sha256"
        Sig      = "$base/hull.sha256.sig"
    }
}

# ── Download + checksum ──────────────────────────────────────────────────────
function Get-HullFile([string]$Url, [string]$OutFile) {
    # Respects the default system proxy. Do not print $Url beyond a generic note.
    Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing -Headers @{ 'User-Agent' = 'hull-install.ps1' }
}

function Get-HullFileSha256([string]$Path) {
    # .NET SHA-256 (no cmdlet/module dependency); streams the file.
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        return ([System.BitConverter]::ToString($sha.ComputeHash($fs)) -replace '-', '').ToLower()
    } finally { $fs.Dispose() }
}

function Get-HullManifestHash([string]$ManifestPath, [string]$AssetName) {
    # Return the single expected hash for $AssetName. Fail closed on a missing,
    # duplicate, or malformed entry.
    $lines = Get-Content -LiteralPath $ManifestPath
    $matched = @()
    foreach ($ln in $lines) {
        # Format: <64 lowercase hex><two spaces><asset-name>
        if ($ln -match '^([0-9a-fA-F]{64})\s\s(.+)$') {
            if ($Matches[2] -ceq $AssetName) { $matched += $Matches[1].ToLower() }
        }
    }
    if ($matched.Count -eq 0) { throw "no checksum entry for '$AssetName' in hull.sha256" }
    if ($matched.Count -gt 1) { throw "duplicate checksum entries for '$AssetName' in hull.sha256" }
    return $matched[0]
}

function Test-HullChecksum([string]$Path, [string]$Expected) {
    $actual = Get-HullFileSha256 $Path
    if ($actual -ne $Expected) {
        throw "checksum mismatch: expected $Expected, got $actual"
    }
    return $actual
}

# ── Upgrade signature check (strict three-outcome contract) ──────────────────
# Bootstrap trust is permitted ONLY when there is no existing hull or the
# existing hull genuinely lacks 'verify-release'. Once an existing hull is proven
# verifier-capable, a missing / unfetched / unparseable / invalid signature MUST
# abort - never a silent downgrade to checksum-only.
function Test-HullVerifierCapable([string]$ExistingHull) {
    if (-not $ExistingHull -or -not (Test-Path -LiteralPath $ExistingHull)) { return $false }
    # Probe without letting the native command's stderr become a terminating error.
    $prevEA = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        $out = & $ExistingHull verify-release --help 2>&1
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $prevEA }
    return ($code -eq 0 -and (($out | Out-String) -match 'verify-release'))
}

function Assert-HullSignature([string]$ExistingHull, [string]$ManifestPath, [string]$SigPath) {
    # Precondition: the caller has established $ExistingHull is verifier-capable.
    # ANY failure here throws (abort); there is no bootstrap fallback.
    $sigItem = Get-Item -LiteralPath $SigPath -ErrorAction SilentlyContinue
    if (-not $sigItem -or $sigItem.Length -eq 0) {
        throw "release signature (hull.sha256.sig) is missing or empty; aborting (the existing hull can verify signatures, so a signature is required)"
    }
    $prevEA = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        $out = & $ExistingHull verify-release $ManifestPath $SigPath 2>&1
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $prevEA }
    if ($code -ne 0) {
        throw "release signature did not verify; aborting (output: $(($out | Out-String).Trim()))"
    }
}

# ── User PATH (HKCU only; idempotent; type-preserving) ───────────────────────
function Get-HullUserPathRaw {
    # Raw (unexpanded) value + its registry kind, so we can preserve REG_EXPAND_SZ.
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
    # Normalize for COMPARISON only (the raw component is always preserved when
    # writing): strip surrounding quotes and whitespace, expand environment
    # variables (so %LOCALAPPDATA%\Programs\Hull matches its expansion), unify
    # separators, drop trailing separators, and lowercase.
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

function Send-HullEnvBroadcast {
    # Notify running shells that the environment changed. Best-effort.
    if (-not ('HullNative.Win32' -as [type])) {
        Add-Type -Namespace HullNative -Name Win32 -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Auto)]
public static extern System.IntPtr SendMessageTimeout(System.IntPtr hWnd, uint Msg, System.UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out System.UIntPtr lpdwResult);
'@ -ErrorAction SilentlyContinue
    }
    try {
        $HWND_BROADCAST = [System.IntPtr]0xffff
        $WM_SETTINGCHANGE = 0x1a
        $SMTO_ABORTIFHUNG = 0x2
        $out = [System.UIntPtr]::Zero
        [void][HullNative.Win32]::SendMessageTimeout($HWND_BROADCAST, $WM_SETTINGCHANGE, [System.UIntPtr]::Zero, 'Environment', $SMTO_ABORTIFHUNG, 5000, [ref]$out)
    } catch { }
}

function Add-HullToUserPath([string]$Dir, [bool]$DryRun) {
    $cur = Get-HullUserPathRaw
    if (Test-HullPathContains $cur.Value $Dir) {
        Write-HullInfo "PATH already contains $Dir"
        return $false
    }
    if ($DryRun) { Write-HullInfo "[dry-run] would add $Dir to user PATH"; return $false }
    $newValue = if ([string]::IsNullOrEmpty($cur.Value)) { $Dir } else { ($cur.Value.TrimEnd(';') + ';' + $Dir) }
    Set-HullUserPathRaw $newValue $cur.Kind
    Send-HullEnvBroadcast
    Write-HullInfo "added $Dir to user PATH"
    return $true
}

function Remove-HullFromUserPath([string]$Dir, [bool]$DryRun) {
    $cur = Get-HullUserPathRaw
    if (-not (Test-HullPathContains $cur.Value $Dir)) { return $false }
    if ($DryRun) { Write-HullInfo "[dry-run] would remove $Dir from user PATH"; return $false }
    $want = ConvertTo-HullPathKey $Dir
    $kept = @()
    foreach ($c in ($cur.Value -split ';')) {
        if ($c -eq '') { continue }
        if ((ConvertTo-HullPathKey $c) -ne $want) { $kept += $c }
    }
    Set-HullUserPathRaw ($kept -join ';') $cur.Kind
    Send-HullEnvBroadcast
    Write-HullInfo "removed $Dir from user PATH"
    return $true
}

# ── Prefix + version-of-installed ────────────────────────────────────────────
function Resolve-HullPrefix([string]$Requested) {
    if ($Requested) { return $Requested }
    $localApp = $env:LOCALAPPDATA
    if (-not $localApp) { $localApp = Join-Path $env:USERPROFILE 'AppData\Local' }
    return (Join-Path $localApp 'Programs\Hull')
}

function Get-HullInstalledVersion([string]$BinPath) {
    if (-not (Test-Path -LiteralPath $BinPath)) { return $null }
    try {
        $v = (& $BinPath version 2>$null | Select-Object -First 1)
        if ($v -match '([0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?)') { return $Matches[1] }
    } catch { }
    return $null
}

function Get-HullArtifactVersion([string]$ExePath, [int]$TimeoutSec = 20) {
    # Run '<exe> version' with a bounded timeout and return the parsed version.
    # Returns $null on timeout / failure / unparseable output. Never hangs.
    $outFile = [System.IO.Path]::GetTempFileName()
    $errFile = [System.IO.Path]::GetTempFileName()
    try {
        $p = Start-Process -FilePath $ExePath -ArgumentList 'version' -PassThru -NoNewWindow `
                           -RedirectStandardOutput $outFile -RedirectStandardError $errFile
        if (-not $p.WaitForExit($TimeoutSec * 1000)) {
            try { $p.Kill() } catch { }
            return $null
        }
        $line = (Get-Content -LiteralPath $outFile | Select-Object -First 1)
        if ($line -match '([0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?)') { return $Matches[1] }
        return $null
    } catch {
        return $null
    } finally {
        Remove-Item -LiteralPath $outFile, $errFile -Force -ErrorAction SilentlyContinue
    }
}

# ── Installer ownership marker (so uninstall only touches what it installed) ──
function Get-HullMarkerPath([string]$PrefixDir) { return (Join-Path $PrefixDir '.hull-install.json') }

function Write-HullMarker([string]$PrefixDir, [string]$InstalledVersion, [bool]$PathAdded) {
    $rec = [ordered]@{
        installer = 'install.ps1'
        prefix    = $PrefixDir
        version   = $InstalledVersion
        pathAdded = $PathAdded
    }
    Set-Content -LiteralPath (Get-HullMarkerPath $PrefixDir) -Value ($rec | ConvertTo-Json) -Encoding ASCII
}

function Read-HullMarker([string]$PrefixDir) {
    $m = Get-HullMarkerPath $PrefixDir
    if (-not (Test-Path -LiteralPath $m)) { return $null }
    try { return (Get-Content -LiteralPath $m -Raw | ConvertFrom-Json) } catch { return $null }
}

# ── Atomic install with rollback ─────────────────────────────────────────────
function Install-HullAtomicMove([string]$Src, [string]$Dest, [switch]$SimulateFailure, [switch]$SimulateRestoreFailure) {
    # Stage the new binary beside the destination under UNIQUE names (so an
    # unrelated pre-existing sidecar is never clobbered), back up any existing
    # binary, move the new one into place, and drop the backup. On ANY failure,
    # restore the previous binary and PROVE it is back; if restoration fails,
    # raise a distinct CRITICAL error naming the preserved backup. The two
    # -Simulate* switches are test-only injections for the rollback paths.
    $dir = Split-Path -Parent $Dest
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $staged = Join-Path $dir (".hull-stage-" + [guid]::NewGuid().ToString('N'))
    $backup = Join-Path $dir (".hull-backup-" + [guid]::NewGuid().ToString('N'))
    if (Test-Path -LiteralPath $staged) { throw "staging path already exists: $staged" }
    if (Test-Path -LiteralPath $backup) { throw "backup path already exists: $backup" }
    Copy-Item -LiteralPath $Src -Destination $staged -Force
    $hadPrev = Test-Path -LiteralPath $Dest
    try {
        if ($hadPrev) { Move-Item -LiteralPath $Dest -Destination $backup -Force }
        if ($SimulateFailure) { throw "simulated install failure (test)" }
        Move-Item -LiteralPath $staged -Destination $Dest -Force
        if ($hadPrev) { Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue }
    } catch {
        $installErr = $_
        Remove-Item -LiteralPath $staged -Force -ErrorAction SilentlyContinue
        if ($hadPrev) {
            if (-not $SimulateRestoreFailure -and -not (Test-Path -LiteralPath $Dest)) {
                Move-Item -LiteralPath $backup -Destination $Dest -Force -ErrorAction SilentlyContinue
            }
            if (-not (Test-Path -LiteralPath $Dest)) {
                throw "CRITICAL: install failed AND rollback failed; your previous hull is preserved at $backup"
            }
            Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue
        }
        throw $installErr
    }
}

# ── Install / uninstall ──────────────────────────────────────────────────────
function Set-HullOwnershipMarker([string]$PrefixDir, [string]$Ver, [bool]$AddedNow) {
    # pathAdded is sticky: once THIS installer added the PATH entry, keep it true
    # across re-installs so a later uninstall still removes exactly what we added.
    $prev = Read-HullMarker $PrefixDir
    $pathAdded = [bool]($AddedNow -or ($prev -and $prev.pathAdded))
    Write-HullMarker $PrefixDir $Ver $pathAdded
}

function Invoke-HullInstall {
    param([string]$Version, [string]$Prefix, [bool]$Force, [bool]$DryRun, [bool]$NoPath)
    $ErrorActionPreference = 'Stop'

    Set-HullTls
    $tag = Resolve-HullTag $Version           # metadata resolution is allowed in dry-run
    $urls = Get-HullAssetUrls $tag
    $prefixDir = Resolve-HullPrefix $Prefix
    $dest = Join-Path $prefixDir $script:HullBinName
    $wantVer = $tag.TrimStart('v')

    Write-HullInfo "repo:    $($script:HullRepo)"
    Write-HullInfo "version: $tag"
    Write-HullInfo "prefix:  $prefixDir"

    # DRY-RUN: resolve metadata and print the plan; touch NO disk (no temp dir, no
    # download, no prefix, no PATH).
    if ($DryRun) {
        Write-HullWarn "dry-run: no files, downloads, or PATH changes will be made"
        Write-HullInfo "[dry-run] would download $($script:HullAsset) for $tag and verify its SHA-256"
        Write-HullInfo "[dry-run] would install to $dest"
        if (-not $NoPath) { Write-HullInfo "[dry-run] would ensure $prefixDir is on the user PATH" }
        return
    }

    # Same-version / overwrite rule.
    $installedVer = Get-HullInstalledVersion $dest
    if ((Test-Path -LiteralPath $dest) -and -not $Force) {
        if ($installedVer -eq $wantVer) {
            Write-HullInfo "hull $installedVer already installed at $dest"
            $added = $false
            if (-not $NoPath) { $added = Add-HullToUserPath $prefixDir $false }
            Set-HullOwnershipMarker $prefixDir $installedVer $added
            return
        }
        throw "a different hull ($installedVer) is already installed at $dest; use -Force to replace it"
    }

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("hull-install-" + [guid]::NewGuid())
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    try {
        # Stage the download AS hull.com so it is runnable for the candidate check.
        $asset = Join-Path $tmp $script:HullBinName
        $man   = Join-Path $tmp 'hull.sha256'
        $sig   = Join-Path $tmp 'hull.sha256.sig'

        Write-HullInfo "downloading $($script:HullAsset)"
        Get-HullFile $urls.Asset $asset
        Get-HullFile $urls.Manifest $man
        if (-not (Test-Path -LiteralPath $asset) -or (Get-Item $asset).Length -eq 0) {
            throw "download produced an empty file; release may not exist at $tag"
        }

        # 1. checksum against the exact manifest entry for hull-cosmo.
        $expected = Get-HullManifestHash $man $script:HullAsset
        $actual = Test-HullChecksum $asset $expected
        Write-HullInfo "checksum verified ($actual)"

        # 2. upgrade signature (strict). A verifier-capable existing hull makes the
        #    signature MANDATORY: a download failure, missing, unparseable, or
        #    invalid signature all abort. No existing hull, or one lacking
        #    verify-release, proceeds under bootstrap trust.
        if (Test-Path -LiteralPath $dest) {
            if (Test-HullVerifierCapable $dest) {
                Get-HullFile $urls.Sig $sig            # a download failure THROWS -> abort
                Assert-HullSignature $dest $man $sig   # missing / invalid THROWS -> abort
                Write-HullInfo "existing hull verified the release signature"
            } else {
                Write-HullWarn "existing hull lacks 'verify-release'; proceeding under bootstrap trust (GitHub HTTPS + matched SHA-256)"
            }
        } else {
            Write-HullWarn "first install: bootstrap trust (GitHub HTTPS + matched SHA-256). Verify further with 'hull verify-release' and Sigstore/Rekor."
        }

        # 3. candidate-version check (after checksum, before replacement): the
        #    verified artifact must report the requested version, else abort
        #    WITHOUT touching the existing installation.
        $candVer = Get-HullArtifactVersion $asset
        if ($candVer -ne $wantVer) {
            throw "downloaded artifact reports version '$candVer', expected '$wantVer'; aborting without changing the existing installation"
        }
        Write-HullInfo "artifact version check: $candVer"

        # 4. install atomically with rollback.
        Install-HullAtomicMove $asset $dest
        Write-HullInfo "installed hull: $dest"

        # 5. PATH + ownership marker.
        $added = $false
        if (-not $NoPath) { $added = Add-HullToUserPath $prefixDir $false }
        Set-HullOwnershipMarker $prefixDir $wantVer $added

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
    $marker = Get-HullMarkerPath $prefixDir
    $rec = Read-HullMarker $prefixDir

    # Ownership gate: only remove what THIS installer recorded installing. This
    # prevents `-Uninstall -Prefix <arbitrary>` from deleting an unrelated hull.com
    # and prevents removing a PATH entry the user owned.
    if (-not $rec) {
        Write-HullInfo "no installer ownership record at $prefixDir; refusing to remove anything not installed by install.ps1"
        return
    }

    if (Test-Path -LiteralPath $dest) {
        if ($DryRun) { Write-HullInfo "[dry-run] would remove $dest" }
        else { Remove-Item -LiteralPath $dest -Force; Write-HullInfo "removed $dest" }
    } else {
        Write-HullInfo "no hull found at $dest"
    }

    if ($rec.pathAdded) { [void](Remove-HullFromUserPath $prefixDir $DryRun) }
    else { Write-HullInfo "user PATH not modified (installer did not add $prefixDir)" }

    if (-not $DryRun) {
        Remove-Item -LiteralPath $marker -Force -ErrorAction SilentlyContinue
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
        if ($Uninstall) {
            Invoke-HullUninstall -Prefix $Prefix -DryRun:$DryRun.IsPresent
        } else {
            Invoke-HullInstall -Version $Version -Prefix $Prefix -Force:$Force.IsPresent -DryRun:$DryRun.IsPresent -NoPath:$NoPath.IsPresent
        }
    } catch {
        Write-HullErr $_.Exception.Message
        exit 1
    }
}

# Dot-sourcing (`. .\install.ps1`) defines the functions WITHOUT running main, so
# the Pester tests can exercise them with local fixtures. Normal execution and
# `irm | iex` run main.
if ($MyInvocation.InvocationName -ne '.') {
    Invoke-HullInstallerMain
}
