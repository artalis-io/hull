#!/bin/sh
# Hull installer. Downloads the latest release binary from GitHub.
#
#   curl -fsSL https://raw.githubusercontent.com/artalis-io/hull/main/install.sh | sh
#
# Environment variables (all optional):
#   HULL_VERSION    Specific version to install (e.g. "v0.1.0"). Default: latest.
#   HULL_PREFIX     Install directory. Default: $HOME/.local/bin (or /usr/local/bin if root).
#   HULL_FORCE      "1" to overwrite existing hull without prompting.
#   HULL_FLAVOR     "cosmo" | "native". Default: native if available for the platform, else cosmo.
#   HULL_REPO       GitHub repo (org/name). Default: artalis-io/hull.
#   HULL_DRY_RUN    "1" to print what would happen without doing it.
#
# Exit codes:
#   0  installed (or dry-run)
#   1  install failed
#   2  prerequisites missing (curl/wget, shasum/sha256sum)
#   3  platform not supported
#
# SPDX-License-Identifier: AGPL-3.0-or-later

set -eu

# ── Configuration ───────────────────────────────────────────────────

HULL_REPO="${HULL_REPO:-artalis-io/hull}"
HULL_VERSION="${HULL_VERSION:-latest}"
HULL_FORCE="${HULL_FORCE:-0}"
HULL_DRY_RUN="${HULL_DRY_RUN:-0}"
HULL_FLAVOR="${HULL_FLAVOR:-auto}"

# ── Pretty output ────────────────────────────────────────────────────

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    BOLD="$(printf '\033[1m')"
    DIM="$(printf '\033[2m')"
    RED="$(printf '\033[31m')"
    GREEN="$(printf '\033[32m')"
    YELLOW="$(printf '\033[33m')"
    BLUE="$(printf '\033[34m')"
    RESET="$(printf '\033[0m')"
else
    BOLD=""; DIM=""; RED=""; GREEN=""; YELLOW=""; BLUE=""; RESET=""
fi

info()  { printf '%s==>%s %s\n' "$BLUE" "$RESET" "$*"; }
ok()    { printf '%s✓%s   %s\n' "$GREEN" "$RESET" "$*"; }
warn()  { printf '%s!%s   %s\n' "$YELLOW" "$RESET" "$*" >&2; }
err()   { printf '%s✗%s   %s\n' "$RED" "$RESET" "$*" >&2; }
dim()   { printf '%s%s%s\n' "$DIM" "$*" "$RESET"; }

run() {
    if [ "$HULL_DRY_RUN" = "1" ]; then
        printf '%s$ %s%s\n' "$DIM" "$*" "$RESET"
    else
        eval "$@"
    fi
}

# ── Prerequisites ────────────────────────────────────────────────────

DOWNLOADER=""
if command -v curl >/dev/null 2>&1; then
    DOWNLOADER="curl -fsSL"
elif command -v wget >/dev/null 2>&1; then
    DOWNLOADER="wget -qO-"
else
    err "neither curl nor wget found. Please install one"
    exit 2
fi

SHA256=""
if command -v sha256sum >/dev/null 2>&1; then
    SHA256="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
    SHA256="shasum -a 256"
else
    warn "neither sha256sum nor shasum found. Checksum verification will be skipped"
fi

# ── Platform detection ───────────────────────────────────────────────

detect_platform() {
    OS=$(uname -s 2>/dev/null || echo unknown)
    ARCH=$(uname -m 2>/dev/null || echo unknown)

    case "$OS" in
        Linux)   OS_NAME=linux ;;
        Darwin)  OS_NAME=darwin ;;
        FreeBSD|OpenBSD|NetBSD|MINGW*|MSYS*|CYGWIN*)
                 OS_NAME=other ;;
        *)       OS_NAME=other ;;
    esac

    case "$ARCH" in
        x86_64|amd64)  ARCH_NAME=x86_64 ;;
        aarch64|arm64) ARCH_NAME=arm64 ;;
        *)             ARCH_NAME=other ;;
    esac
}

# Map (OS, ARCH, flavor) → asset name (matches release.yml output).
#
# Available assets:
#   hull-cosmo            (universal APE: linux/darwin/windows/bsd × x86_64/arm64)
#   hull-linux-x86_64     (native ELF, x86_64)
#   hull-linux-aarch64    (native ELF, ARM64 — Graviton, DGX, Ampere, Pi 4+)
#   hull-darwin-arm64     (native Mach-O)
select_asset() {
    if [ "$HULL_FLAVOR" = "cosmo" ]; then
        ASSET=hull-cosmo
        return
    fi

    if [ "$HULL_FLAVOR" = "native" ]; then
        case "${OS_NAME}-${ARCH_NAME}" in
            linux-x86_64)  ASSET=hull-linux-x86_64 ;;
            linux-arm64)   ASSET=hull-linux-aarch64 ;;
            darwin-arm64)  ASSET=hull-darwin-arm64 ;;
            *) err "no native binary for ${OS_NAME}-${ARCH_NAME}. Set HULL_FLAVOR=cosmo"
               exit 3 ;;
        esac
        return
    fi

    # auto: prefer native, fall back to cosmo APE
    case "${OS_NAME}-${ARCH_NAME}" in
        linux-x86_64)  ASSET=hull-linux-x86_64 ;;
        linux-arm64)   ASSET=hull-linux-aarch64 ;;
        darwin-arm64)  ASSET=hull-darwin-arm64 ;;
        darwin-x86_64|other-*|*-other)
            ASSET=hull-cosmo ;;
        *) ASSET=hull-cosmo ;;
    esac
}

# ── Resolve release URLs ─────────────────────────────────────────────

resolve_version() {
    if [ "$HULL_VERSION" = "latest" ]; then
        info "resolving latest release for $HULL_REPO …"
        # GitHub redirects /releases/latest to /releases/tag/<tag>
        latest_url=$($DOWNLOADER "https://api.github.com/repos/${HULL_REPO}/releases/latest" \
                      2>/dev/null \
                    | grep -E '"tag_name":' \
                    | head -1 \
                    | sed -E 's/.*"tag_name": *"([^"]+)".*/\1/')
        if [ -z "$latest_url" ]; then
            err "could not find any release at https://github.com/${HULL_REPO}/releases"
            err "(if this is a fresh repo, no release has been tagged yet)"
            err "set HULL_VERSION=vX.Y.Z to install a specific version once available"
            exit 1
        fi
        HULL_VERSION="$latest_url"
    fi

    BASE_URL="https://github.com/${HULL_REPO}/releases/download/${HULL_VERSION}"
    ASSET_URL="${BASE_URL}/${ASSET}"
    CHECKSUM_URL="${BASE_URL}/hull.sha256"
}

# ── Install location ─────────────────────────────────────────────────

resolve_prefix() {
    if [ -n "${HULL_PREFIX:-}" ]; then
        # User-specified. Use as-is
        return
    fi

    if [ "$(id -u 2>/dev/null || echo 1000)" = "0" ]; then
        HULL_PREFIX="/usr/local/bin"
    else
        HULL_PREFIX="${HOME}/.local/bin"
    fi
}

# ── Download + verify + install ──────────────────────────────────────

download_and_install() {
    TMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/hull-install.XXXXXX")
    # shellcheck disable=SC2064
    trap "rm -rf '$TMPDIR'" EXIT INT TERM

    info "downloading ${ASSET}"
    dim "        from ${ASSET_URL}"
    run "$DOWNLOADER '$ASSET_URL' > '$TMPDIR/$ASSET'"

    if [ "$HULL_DRY_RUN" != "1" ] && [ ! -s "$TMPDIR/$ASSET" ]; then
        err "download produced an empty file. Release likely does not exist at $HULL_VERSION"
        exit 1
    fi

    if [ -n "$SHA256" ]; then
        info "verifying SHA-256 checksum"
        if ! run "$DOWNLOADER '$CHECKSUM_URL' > '$TMPDIR/hull.sha256'"; then
            warn "could not fetch checksum file. Proceeding without verification"
        else
            if [ "$HULL_DRY_RUN" != "1" ]; then
                expected=$(grep " $ASSET\$" "$TMPDIR/hull.sha256" | awk '{print $1}')
                actual=$(cd "$TMPDIR" && $SHA256 "$ASSET" | awk '{print $1}')
                if [ -z "$expected" ]; then
                    warn "no checksum entry for $ASSET in hull.sha256. Skipping verification"
                elif [ "$expected" != "$actual" ]; then
                    err "checksum mismatch:"
                    err "  expected: $expected"
                    err "  actual:   $actual"
                    exit 1
                else
                    ok "checksum verified ($actual)"
                fi
            fi
        fi
    fi

    DEST="${HULL_PREFIX}/hull"

    if [ -e "$DEST" ] && [ "$HULL_FORCE" != "1" ] && [ "$HULL_DRY_RUN" != "1" ]; then
        warn "$DEST already exists"
        if [ -t 0 ]; then
            printf "overwrite? [y/N] "
            read -r reply
            case "$reply" in
                y|Y|yes|YES) ;;
                *) info "aborted (set HULL_FORCE=1 to skip this prompt)"; exit 0 ;;
            esac
        else
            warn "non-interactive. Refusing to overwrite (set HULL_FORCE=1 to override)"
            exit 1
        fi
    fi

    info "installing to $DEST"
    run "mkdir -p '$HULL_PREFIX'"
    run "mv '$TMPDIR/$ASSET' '$DEST'"
    run "chmod +x '$DEST'"

    ok "hull installed: $DEST"
}

# ── Post-install hints ───────────────────────────────────────────────

post_install() {
    if [ "$HULL_DRY_RUN" = "1" ]; then
        return
    fi

    # Check PATH
    case ":$PATH:" in
        *":$HULL_PREFIX:"*) ;;
        *)
            warn "$HULL_PREFIX is not in PATH"
            shell_name=$(basename "${SHELL:-/bin/sh}")
            case "$shell_name" in
                bash) rcfile="$HOME/.bashrc" ;;
                zsh)  rcfile="$HOME/.zshrc" ;;
                fish) rcfile="$HOME/.config/fish/config.fish" ;;
                *)    rcfile="your shell rc file" ;;
            esac
            printf '\nAdd this line to %s:\n\n' "$rcfile"
            if [ "$shell_name" = "fish" ]; then
                printf '    %sfish_add_path %s%s\n\n' "$BOLD" "$HULL_PREFIX" "$RESET"
            else
                printf '    %sexport PATH="%s:$PATH"%s\n\n' "$BOLD" "$HULL_PREFIX" "$RESET"
            fi
            ;;
    esac

    # Print version (sanity check the binary runs)
    if command -v hull >/dev/null 2>&1 || [ -x "$HULL_PREFIX/hull" ]; then
        hull_bin="${HULL_PREFIX}/hull"
        [ -x "$hull_bin" ] || hull_bin=$(command -v hull)
        version=$("$hull_bin" version 2>/dev/null | head -1 || echo "(version check failed)")
        printf '\n%s\n' "$version"
    fi

    printf '\n'
    info "next steps:"
    printf '  %shull doctor%s         . Check environment\n' "$BOLD" "$RESET"
    printf '  %shull init myapp%s     . Create a project\n' "$BOLD" "$RESET"
    printf '  %shull dev app.lua%s    . Run the dev server\n' "$BOLD" "$RESET"
    printf '  %shull update%s         . Install future releases\n' "$BOLD" "$RESET"
    printf '\n'
}

# ── Main ─────────────────────────────────────────────────────────────

main() {
    printf '\n%sHull installer%s\n\n' "$BOLD" "$RESET"

    detect_platform
    select_asset
    resolve_version
    resolve_prefix

    info "platform:  ${OS_NAME}-${ARCH_NAME}"
    info "asset:     ${ASSET}"
    info "version:   ${HULL_VERSION}"
    info "prefix:    ${HULL_PREFIX}"
    [ "$HULL_DRY_RUN" = "1" ] && warn "dry-run mode. No files will be written"
    printf '\n'

    download_and_install
    post_install
}

main "$@"
