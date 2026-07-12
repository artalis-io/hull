# Reproducible build container (roadmap 0.3.1)

This directory holds a **registry-less, snapshot-pinned** build environment used
by the reproducibility-critical Linux and cosmo CI/release jobs. It closes the
"versioned runner images still get monthly patch updates" gap so that
"reproducible across time" holds indefinitely for the Linux and cosmo artifacts,
not just within a major-version window.

Design rationale (and why this over a published GHCR image or a Nix flake) lives
in [`docs/security.md` → "Build-environment immutability"](../../docs/security.md).

## How it works

- **`Dockerfile.build`** pins two things:
  1. the base image by manifest-list digest (`FROM ubuntu:24.04@sha256:...`), and
  2. the apt archive to a `snapshot.ubuntu.com` point-in-time via
     `repro-sources-list.sh`.

  With both pinned, `apt-get install` resolves byte-identical
  `gcc` / `glibc` / `binutils` on every rebuild, so a `hull` built inside is
  byte-reproducible across time. The SHA-pinned cosmocc is baked in the same way.

- **`repro-sources-list.sh`** is vendored verbatim from upstream (see provenance
  below). It rewrites the apt sources to the snapshot, covering the
  `main` / `restricted` / `universe` / `multiverse` / `-updates` / `-security` /
  `-backports` pockets, and derives `SOURCE_DATE_EPOCH` from the (digest-pinned,
  hence constant) base-image mtime unless one is passed explicitly.

- **`.github/actions/hull-build-container`** is the composite action jobs use:
  it `docker build`s the image, then `docker run`s a command with the repo
  bind-mounted at `/src`. The `container:` job key is deliberately not used
  because it requires a pre-existing image; a registry-less image built in-job
  must be run via `docker run`.

macOS is out of scope: `hull-darwin-arm64` builds on a real `macos-15` runner
with Xcode, which no container touches. Its reproducibility stays bounded by the
`macos-15` image patch window.

## Vendored dependency provenance

| File | Upstream | Version | SHA-256 |
|------|----------|---------|---------|
| `repro-sources-list.sh` | [reproducible-containers/repro-sources-list.sh](https://github.com/reproducible-containers/repro-sources-list.sh) | `v0.1.4` (commit `39fbf150e3a5062d4c6b9a241f25af133e7cb6f0`) | `c125df9762b0c7233459087bb840c0e5dbfc4d9690ee227f1ed8994f4d51d2e0` |

The script is Apache-2.0 licensed; its header is retained verbatim.

## Bump procedures

**Base image digest.** Re-pin `FROM ubuntu:24.04@sha256:...` deliberately (it
re-pins the entire toolchain). Get the current multi-arch manifest-list digest:

```sh
docker buildx imagetools inspect ubuntu:24.04 | grep -i '^Digest:'
```

Bumping the digest changes the derived apt-snapshot timestamp and therefore the
frozen toolchain. That is expected: it is the reviewed act of moving the pin.

**cosmocc.** Keep the `COSMOCC_VERSION` / `COSMOCC_SHA256` build args in
`Dockerfile.build` in sync with the Makefile's `COSMOCC_VERSION` /
`COSMOCC_SHA256`.

**`repro-sources-list.sh`.** Re-vendor at a pinned tag and update the table
above:

```sh
curl -fsSL https://raw.githubusercontent.com/reproducible-containers/repro-sources-list.sh/<TAG>/repro-sources-list.sh \
  -o .github/docker/repro-sources-list.sh
shasum -a 256 .github/docker/repro-sources-list.sh
```
