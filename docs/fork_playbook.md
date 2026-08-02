# Hull Fork Playbook

**Status:** Authoritative for v0.1.5+.
**Audience:** anyone forking Hull to ship their own trust-rooted distribution. Sovereign deployments, defense primes, regulated environments, vendor-specific bundles. Not an end-user document.

Hull is AGPL-3.0-or-later. The license grants you the right to fork and redistribute under your own name and your own trust root. This document walks through the actual procedure: rotate the two embedded public keys, fork the release pipeline, replace the install URL, regenerate the installer, run your own CI, ship.

The whole operation is ~half a day of focused work for one engineer. The hard part is operational discipline (key storage, signing ceremony), not code.

---

## When you'd fork

You should fork Hull (instead of running upstream binaries) if any of these apply:

- **Sovereign trust.** Your deployment policy says "no foreign trust roots in the supply chain" and the embedded `gethull.dev` release pubkey is a non-starter.
- **Compliance ban on third-party signers.** Your auditor requires that every binary you run be signed by a key your organisation controls.
- **Air-gapped or vendor-specific distribution.** You need `acme update` to pull from `releases.acme.internal`, not from `github.com/artalis-io/hull`.
- **Custom feature set baked in.** You ship a build with `HL_ENABLE_GPU=0` and `HL_ENABLE_DB=0` by default and you want that to be the canonical "acme-hull" binary.

If you just want to *use* Hull with your own apps, you do not need to fork. Run upstream `hull`, ship apps signed by your own developer key. Forking is only required when you want to replace the runtime's trust root itself.

---

## What forking gets you, and what it doesn't

**Gets you:**

- A complete trust chain rooted in keys *your* organisation controls.
- `your-hull update` only accepts updates signed by your release key.
- `your-hull build` produces apps whose `package.sig` chains to your platform key.
- A self-hosted release endpoint at a URL you control.

**Does not get you:**

- Independence from upstream Hull's source. You still inherit AGPL obligations (any modifications you ship as a network service must be source-available).
- Independence from upstream Hull's vendored dependencies. mbedTLS, Lua, QuickJS, SQLite, WAMR, etc. are still in your tree; the fork doesn't change *their* trust posture.
- Automatic upstream merges. You take on the work of pulling in upstream fixes (security or otherwise) and re-signing your release.
- Use of the "Hull" name or `gethull.dev` branding. AGPL grants copyright permissions, not trademark permissions. Your fork ships under a new name, and any "based on Hull" attribution must not imply official endorsement.

---

## Why most organisations shouldn't fork

The playbook below works. The legal right is real. But forking is a substantially larger commitment than it looks at first glance, and most teams who think they want to fork actually want something simpler. Before going further, read this section honestly.

**1. AGPL §13 obligations apply to your fork too.**
The AGPL is what gives you the right to fork. It also propagates: anyone running your fork as a network service must offer source code to their network users. If your customers are the kind of organisation that bought your fork specifically because they didn't want AGPL exposure, they will discover this clause the moment legal review notices it. The fork doesn't escape AGPL; it just moves the obligation to you and your downstream.

**2. You inherit the full security posture.**
Every CVE in mbedTLS, Lua, QuickJS, SQLite, WAMR, miniz, stb, and the rest of the vendored dependency tree becomes your problem. Upstream Hull tracks these and ships patched releases. Your fork has to do the same. Realistically: budget one engineer-month per year just for security-advisory monitoring + patch porting + re-release work, before you've shipped any features.

**3. You become the support contract for your fork's users.**
When a customer's `acme-hull` segfaults at 2am, they call you, not gethull.dev. They are not entitled to upstream support, and upstream is not aware your fork exists. You are now the L3 escalation for a runtime + capability layer + sandbox + crypto + database + WASM engine + GPU shim + (possibly) WAMR AOT, on Linux + macOS + Cosmopolitan. That's a real on-call rotation. Hope you priced it in.

**4. Upstream divergence cost is super-linear.**
The maintenance section below describes the merge strategies. None of them are free. Six months of drift is recoverable with a long afternoon; eighteen months becomes a project; three years and you're effectively maintaining a separate codebase that happens to share ancestry. Most forks underestimate this and end up either (a) abandoning upstream sync and slowly falling behind on security fixes, or (b) over-committing engineering time to staying current.

**5. Trust is non-transitive.**
Your customers are buying *your* trust root now, not gethull's. That sounds like the whole point, but it means: your key custody is the threat model, your release ceremony is the threat model, your CI environment is the threat model, your build reproducibility is the threat model. Upstream Hull has spent real engineering on these (see `docs/security.md` + `roadmap_next.md §0.3`). If your operational discipline is weaker than upstream's, your fork is a *worse* trust root for your customers than upstream Hull was. The fork only helps if your security posture meets or exceeds upstream's.

**6. The "we need our own trust root" requirement is often satisfied without a fork.**
Three lighter-weight alternatives, listed in order of how often they actually solve the real problem:

- **Use upstream Hull, sign your apps with your own developer key.** The `package.sig` outer-layer key is already yours; the platform key just proves the runtime is authentic. If your real requirement is "every app we ship is signed by us," upstream Hull already gives you that without a fork. The platform key only matters if you want to deny upstream gethull as a valid runtime.
- **Pin a specific upstream Hull version + verify the SHA-256 yourself.** If your real requirement is supply-chain pinning, `hull verify-release` + a committed copy of `hull.sha256` + `hull.sha256.sig` in your infrastructure gives you that. The trust root is still gethull's; the *version* is yours.
- **Run upstream Hull behind a private hosted-release endpoint.** If your real requirement is "no traffic to github.com from production hosts," mirror the upstream release assets to your internal artifact store and point `--repo` at it. No fork needed.

If after reading all of the above your answer is still "we need our own trust root, our own brand, and we accept the maintenance burden," then fork. The procedure below assumes you mean it.

---

## The five-step procedure

1. **Fork the source.**
2. **Generate two new keypairs** (release + platform), store the private halves offline.
3. **Embed the new public keys** into the source.
4. **Fork the release workflow** (replace secrets, URLs, asset names).
5. **Replace the install URL** in `install.sh` and the docs site.

Each step is detailed below. Skipping any step leaves an upstream-trust hole.

---

### Step 1. Fork the source

Standard GitHub fork or local clone-and-rename. Conventionally:

```sh
git clone https://github.com/artalis-io/hull acme-hull
cd acme-hull
git remote rename origin upstream
git remote add origin https://github.com/acme/acme-hull.git
git push -u origin main
```

Decide on a new binary name (`acme-hull`, `acmectl`, `your-runtime`). For the rest of this doc, `acme-hull` is the placeholder.

If you don't intend to track upstream Hull releases, you can delete `.git/refs/remotes/upstream` and treat this as an independent codebase. If you DO intend to merge upstream fixes, keep the `upstream` remote and read the "Maintenance" section below.

### Step 2. Generate keypairs

Hull's two trust roots:

- **Release key** (signs `hull.sha256`, verified by `hull update`)
- **Platform key** (signs `libhull_platform.a`'s manifest, verified by `hull build` when stitching the inner signature layer)

Both are Ed25519. The upstream build embeds:

- `HL_RELEASE_PUBKEY_HEX` in `include/hull/release.h`
- `HL_PLATFORM_PUBKEY_HEX` in `include/hull/signature.h`

Generate both with the upstream Hull's `hull keygen` (no chicken-and-egg: this is one-time tooling, and the upstream Hull is just generating bytes, not signing anything):

```sh
# Release key
hull keygen release           # writes release_pubkey.hex + release_seckey.hex
# Platform key
hull keygen release           # do twice, rename outputs
mv release_pubkey.hex platform_pubkey.hex
mv release_seckey.hex platform_seckey.hex
```

**Store the secret halves offline.** USB stick in a safe, hardware token (YubiKey OpenPGP / Ed25519), or both. Do not commit them. Do not paste them into chat. The GitHub Actions secret store is the bare minimum. A hardware token is far better. See `docs/release_signing.md` for the upstream key-handling policy you should at minimum match.

Record the public halves where your team can find them. They will be embedded in the binary (visible to anyone running `acme-hull`); they are not secret.

### Step 3. Embed the new public keys

Replace the embedded pubkey hex strings:

```sh
# Replace release pubkey
sed -i.bak \
  's/#define HL_RELEASE_PUBKEY_HEX .*/#define HL_RELEASE_PUBKEY_HEX "<your-64-char-hex>"/' \
  include/hull/release.h

# Replace platform pubkey
sed -i.bak \
  's/#define HL_PLATFORM_PUBKEY_HEX .*/#define HL_PLATFORM_PUBKEY_HEX "<your-64-char-hex>"/' \
  include/hull/signature.h

# Sanity check
grep -E 'PUBKEY_HEX' include/hull/release.h include/hull/signature.h
```

Each `<your-64-char-hex>` is the matching `*_pubkey.hex` content (single line, no newline, exactly 64 lowercase hex chars).

You can also override at build time without touching headers:

```sh
make CFLAGS_EXTRA='-DHL_RELEASE_PUBKEY_HEX=\"<hex>\" -DHL_PLATFORM_PUBKEY_HEX=\"<hex>\"'
```

The header-edit form is preferable for a permanent fork; the build-time form is fine for one-off rebuilds.

### Step 4. Fork the release workflow

Edit `.github/workflows/release.yml`:

- Replace `HULL_RELEASE_KEY` and `HULL_PLATFORM_KEY` secret references with your secrets if you renamed them.
- Replace asset names: `hull-linux-x86_64` → `acme-hull-linux-x86_64`, etc. (Or keep the names if you want existing user tooling to work.)
- Replace the release manifest filename if you want a separate namespace: `hull.sha256` → `acme-hull.sha256`.
- Adjust `gh release create` flags to your repository naming.

In your GitHub repository settings, create the two secrets:

- `HULL_RELEASE_KEY` = contents of `release_seckey.hex`
- `HULL_PLATFORM_KEY` = contents of `platform_seckey.hex`

If you are running on self-hosted infrastructure (GitLab CI, internal Jenkins, etc.), port the workflow accordingly. The relevant primitives:

- `hull sign-release <manifest> --key <seckey-hex>` writes `<manifest>.sig`
- `hull sign-platform <platform.a> --key <seckey-hex>` signs the platform library manifest

Both are pure CPU work, no network. They can run on an air-gapped signer if your threat model demands it.

### Step 5. Replace the install URL

Edit `install.sh`:

- Replace `https://github.com/artalis-io/hull` with `https://github.com/acme/acme-hull` (or your release-asset host).
- Replace `hull-linux-x86_64`, etc. with your asset names if you renamed them in step 4.
- Replace the install path from `~/.local/bin/hull` to `~/.local/bin/acme-hull` if you renamed the binary.

If you host the installer at your own URL:

```sh
# Your end-user install line:
curl -fsSL https://acme.internal/install.sh | sh
```

Update your docs site, README, and any onboarding material. Anywhere `gethull.dev` appears, you'll want your equivalent.

---

## Verification checklist

Before declaring the fork live, run through this list:

- [ ] `acme-hull version` shows your version string (with a fork suffix is conventional: `v0.1.5-acme.1`).
- [ ] `acme-hull doctor` reports your embedded platform-sig pubkey, not gethull's.
- [ ] `acme-hull verify-release acme-hull.sha256 acme-hull.sha256.sig` succeeds against your test release.
- [ ] `acme-hull update --check --repo=acme/acme-hull` finds your release.
- [ ] `acme-hull sbom --format=json | jq .components` still lists the upstream-vendored libraries (mbedTLS, Lua, etc.). Your fork doesn't replace those, just the trust root.
- [ ] An end-user install from your install URL produces a binary whose `binary_sha256` (from `acme-hull sbom`) matches what's in your `acme-hull.sha256` manifest.
- [ ] Your CI builds the release reproducibly (`make reproducible-check` passes against your forked source).
- [ ] An app built with `acme-hull build` verifies under `acme-hull verify` and FAILS under upstream `hull verify` (proves your platform key actually replaced the upstream one).

The last item is the critical one. If upstream `hull verify` accepts your app's signature, you didn't actually rotate the platform key. Re-check step 3.

---

## Maintenance

Upstream Hull will ship security fixes, new capabilities, and bug fixes. You have three options:

1. **Track upstream closely.** Set up a `merge-upstream` workflow that opens a PR every release. Resolve conflicts (mostly in `release.yml` and the install URL), re-test, re-sign, ship.
2. **Cherry-pick security fixes only.** Pin to a Hull version, monitor upstream advisories, port just the relevant commits. Lower velocity, lower merge cost.
3. **Hard fork.** Stop tracking upstream entirely. You take on the full maintenance burden. Don't pick this unless you have a dedicated team.

Most production forks should pick option 1 or 2. The fork divergence cost grows quickly if you let it.

If you let your fork drift more than ~6 months behind upstream, the merge cost becomes substantial enough that re-forking from a fresh upstream snapshot is often cheaper than catching up.

---

## Reverting a fork

If you decide the fork was a mistake (most common reason: maintenance burden exceeded value), the path back is straightforward:

1. Communicate the deprecation to your users.
2. Publish a final fork release containing a note pointing users to upstream Hull's install URL.
3. Optionally ship a one-shot tool that re-points `acme-hull update --repo=...` at upstream and atomically replaces `acme-hull` with upstream `hull`.
4. Archive your fork's GitHub repository.

Don't delete the fork repo. Users still need access to the source per AGPL §13 (network use clause). Archive it instead.

---

## Threat-model honesty

A fork rotates Hull's two trust roots and points the update URL at your infrastructure. It does NOT magically harden every other element of the supply chain. After forking you still inherit the v0.1.5 trust-chain gaps documented in `roadmap_next.md §0.3`:

- CI environment drift (your CI image rotates too).
- Vendored dependency trust (your fork still trusts the same Lua, QuickJS, etc. submodule pins).
- Signing-key custody (your release key has the same threat model as upstream's unless you move it to an HSM).

The fork gives you control of the *root*. The branches above the root are still made of the same wood. Plan to address §0.3 items in your fork on the same priority order recommended for upstream.

---

## See also

- `docs/release_signing.md`. What gets signed, by whom, in what order.
- `docs/security.md §5`. Three-layer signature design.
- `docs/roadmap_next.md §0.3`. Trust-chain hardening roadmap (you will inherit all of these).
- `LICENSING.md`. Vendored-dependency licenses you must comply with in any redistribution.
- `CHANGELOG.md`. Upstream version history; reference point when computing your fork-vs-upstream delta.
