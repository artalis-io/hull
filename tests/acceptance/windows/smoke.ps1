<#
  smoke.ps1 - the minimal artifact smoke body, run AS the standard (non-admin)
  user. Verifies the published hull-cosmo BEFORE execution, then installs cosmocc
  and builds + serves a Lua and a JS /ping app.

  Order (fail-closed; a failed pre-execution verify stops before any build/run):
    1. checksum the downloaded bytes against hull.sha256 (Get-FileHash, NO
       execution - so the bytes are verified before they are ever run);
    2. verify hull.sha256.sig (against the source release pubkey when provided,
       else the artifact's embedded key);
    3. the artifact reports the expected version;
    4. non-admin `hull tools install cosmocc` succeeds;
    5. `hull build` + serve a Lua and a JS /ping app, each returning pong.

  Read-only: no update, rollback, staging, or release/asset mutation.

  Usage: smoke.ps1 -Hull <hull-cosmo> -Manifest <hull.sha256> -Sig <hull.sha256.sig> \
                   -Pubkey <hex?> -ExpectVersion 0.14.0 -Evidence <file>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Hull,
    [Parameter(Mandatory=$true)][string]$Manifest,
    [Parameter(Mandatory=$true)][string]$Sig,
    [string]$Pubkey = '',
    [Parameter(Mandatory=$true)][string]$ExpectVersion,
    [Parameter(Mandatory=$true)][string]$Evidence
)

$ErrorActionPreference = 'Stop'
function Note($m) { Add-Content -Path $Evidence -Value $m; Write-Host $m }
function Fail($m) { Note ("  FAIL: {0}" -f $m); $script:fail = 1 }
# hull writes progress to stdout and logs/errors to stderr; under -EA Stop a
# 2>&1 merge of native stderr becomes a terminating error. Capture through a
# helper that locally relaxes the preference and merges both streams.
function Cap([scriptblock]$sb) { $ErrorActionPreference = 'Continue'; & $sb 2>&1 }
$script:fail = 0
$work = Join-Path $env:TEMP ("hull-smoke-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null

# Serve a BUILT standalone APE and curl a path; returns the response body (or '').
# A built binary IS the app, so it is run directly as `<exe> -p <port>`.
function Serve-And-Get([string]$exe, [int]$port, [string]$route) {
    $so = Join-Path $work ("serve-$port.out.log")
    $se = Join-Path $work ("serve-$port.err.log")
    $p = Start-Process -FilePath $exe -ArgumentList @('-p', "$port") -PassThru `
                       -WindowStyle Hidden -RedirectStandardOutput $so -RedirectStandardError $se
    try {
        for ($i = 0; $i -lt 60; $i++) {
            Start-Sleep -Milliseconds 250
            try { return (Invoke-RestMethod -Uri "http://127.0.0.1:$port$route" -TimeoutSec 2) } catch {}
            if ($p.HasExited) { break }
        }
        Note ("- serve diagnostics for port {0} (exited={1} code={2}):" -f $port, $p.HasExited, $p.ExitCode)
        foreach ($f in @($so, $se)) {
            if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) {
                Get-Content $f | Select-Object -First 20 | ForEach-Object { Note ("    [serve] " + $_) }
            }
        }
        return ''
    } finally { $p | Stop-Process -Force -ErrorAction SilentlyContinue }
}

# `hull build` drives dual-arch cosmocc, whose process spawning is intermittently
# flaky on Windows under repeated heavy use. Retry ONLY spawn-specific signatures;
# a genuine link regression is returned on the first try.
function Build-Retry([string]$dir, [string]$outCom) {
    $flake   = 'posix_spawn|ERROR_KERNEL_APC|Bad file number|cannot execute'
    $hometmp = Join-Path $env:HOME '.hull\tmp'
    $out = $null; $rc = 1
    for ($t = 1; $t -le 3; $t++) {
        Get-ChildItem -Path $hometmp -Filter 'fatcosmocc*' -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
        $out = Cap { & $Hull build $dir -o $outCom }
        $rc  = $LASTEXITCODE
        if ($rc -eq 0) { return @{ rc = 0; out = $out; tries = $t } }
        if (($out | Out-String) -notmatch $flake) { return @{ rc = $rc; out = $out; tries = $t } }
        Note ("- build attempt {0} hit a cosmocc spawn flake; cleaning temp and retrying" -f $t)
        Remove-Item $outCom -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 3
    }
    return @{ rc = $rc; out = $out; tries = 3 }
}

Note "## Published artifact smoke (as $(whoami))"

# --- 1. checksum BEFORE executing the binary -------------------------------
$want = ((Get-Content $Manifest | Where-Object { $_ -match '\shull-cosmo$' } | Select-Object -First 1) -replace '\s.*','').ToLower()
$got  = (Get-FileHash $Hull -Algorithm SHA256).Hash.ToLower()
Note ("- checksum want={0} got={1}" -f $want, $got)
if ($want -and ($want -eq $got)) { Note "- OK: checksum matches hull.sha256 (verified before execution)" }
else { Fail "checksum mismatch (or hull-cosmo absent from hull.sha256)" }

# --- 2. signature ----------------------------------------------------------
$sa = @('verify-release', $Manifest, $Sig)
if ($Pubkey) { $sa += @('--pubkey', $Pubkey) }
$so = Cap { & $Hull @sa }; $sr = $LASTEXITCODE
Note (($so | Out-String) -replace '(?m)^','    ')
if ($sr -eq 0) { Note ("- OK: signature verified" + $(if ($Pubkey) { " (source release pubkey)" } else { " (embedded key)" })) }
else { Fail "hull.sha256.sig did not verify" }

# --- 3. version ------------------------------------------------------------
$ver = (Cap { & $Hull version } | Select-Object -First 1)
Note ("- version: {0}" -f $ver)
if ($ver -match [regex]::Escape($ExpectVersion)) { Note "- OK: reports $ExpectVersion" }
else { Fail "version is not $ExpectVersion" }

# A failed pre-execution verify stops here - never build/run unverified bytes.
if ($script:fail -ne 0) { Note "SMOKE: FAIL (pre-execution verify)"; exit 1 }

# --- 4. non-admin cosmocc install ------------------------------------------
Note "## Non-admin cosmocc install"
$ci = Cap { & $Hull tools install cosmocc }; $rc = $LASTEXITCODE
Note (($ci | Out-String) -replace '(?m)^','    ')
if ($rc -ne 0) { Fail "hull tools install cosmocc returned non-zero (non-admin)" }
else { Note "- OK: cosmocc installed non-admin" }

# --- 5. build + serve minimal Lua and JS /ping -----------------------------
Note "## Build + serve /ping (Lua + JS)"
$lua = @'
app.manifest({ modules = { "hull/http-server@1" } })
app.get("/ping", function(req, res) res:text("pong") end)
'@
$js = @'
import { app } from "hull:app";
app.manifest({ modules: ["hull/http-server@1"] });
app.get("/ping", (req, res) => res.text("pong"));
'@
foreach ($case in @(@{ext='lua';src=$lua;port=39761}, @{ext='js';src=$js;port=39762})) {
    $adir = Join-Path $work ("ping-" + $case.ext)
    New-Item -ItemType Directory -Path $adir | Out-Null
    Set-Content -Path (Join-Path $adir ("app." + $case.ext)) -Value $case.src
    $r = Build-Retry $adir (Join-Path $adir 'out.com')
    if ($r.rc -ne 0) { Note (($r.out | Out-String) -replace '(?m)^','    '); Fail ("hull build failed for the " + $case.ext + " /ping app after " + $r.tries + " attempt(s)"); continue }
    $body = Serve-And-Get (Join-Path $adir 'out.com') $case.port '/ping'
    if ("$body".Trim() -eq 'pong') { Note ("- OK: " + $case.ext + " /ping served pong") }
    else { Fail ($case.ext + " /ping did not serve pong (got '" + $body + "')") }
}

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if ($script:fail -ne 0) { Note "SMOKE: FAIL"; exit 1 }
Note "SMOKE: OK (checksum + signature + version + non-admin cosmocc + Lua/JS pong)"
exit 0
