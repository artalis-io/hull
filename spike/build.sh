#!/bin/sh
# spike/build.sh - self-contained build + run for the SMTP-to-Keel-v3 feasibility
# spike (Slice 2a, EXTENDED). STANDALONE: does NOT touch the production Makefile
# or mk/*.
#
# The PUBLIC-HEADER COMPILE GATE is the include path: ONLY
#   -Ivendor/keel/include                     (the public Keel API)
#   -Ivendor/keel/integrations/tls/mbedtls    (the keel_tls_mbedtls.h header)
# No -Ivendor/keel/src (or any src/ subdir). If the spike needs a src/ header it
# FAILS to compile - which is itself a finding.
#
# PORTABLE. The SAME script runs on Linux and macOS, and a `--cosmo` mode builds
# and runs the spike as a Cosmopolitan APE. The full suite runs on TWO readiness
# backends to prove readiness-portability:
#   1. the PLATFORM DEFAULT backend  -- epoll on Linux, kqueue on macOS -- plain + ASan
#   2. a poll-backend rebuild         -- plain + ASan
# then RESTORES the platform default backend so the tree is left as it started.
#
# The default backend is NEVER hardcoded: the script builds libkeel WITHOUT a
# BACKEND override (so Keel selects the platform default) and then reports the
# ACTUAL backend by inspecting the built archive:
#   ar t vendor/keel/libkeel.a | grep -oE 'event_(epoll|kqueue|poll)\.o'
#
# `--cosmo` mode builds libkeel with CC=cosmocc (cosmocc auto-selects the poll
# backend), compiles the spike with cosmocc under the SAME public-header gate,
# links libkeel.a + cosmocc-built mbedTLS objects + the miniz object (which is
# already inside libkeel.a), and RUNS the resulting APE (poll = readiness). This
# authoritatively exercises the Cosmo APE / poll path.
#
# Completion backends (IOCP / io_uring) are OUT OF SCOPE: Hull ships no native-
# Windows runtime (its Windows target is the Cosmo APE = poll = readiness).
#
# Run from anywhere (cd to the repo root automatically).
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$ROOT"

# ── parse args ────────────────────────────────────────────────────────────────
COSMO=0
for arg in "$@"; do
    case "$arg" in
        --cosmo) COSMO=1 ;;
        *) echo "usage: sh spike/build.sh [--cosmo]" >&2; exit 2 ;;
    esac
done

CC=${CC:-cc}
AR=${AR:-ar}

KEEL_DIR="vendor/keel"
KEEL_LIB="$KEEL_DIR/libkeel.a"
MBEDTLS_DIR="vendor/mbedtls"
MINIZ_DIR="vendor/miniz"
TLS_INT_OBJ="$KEEL_DIR/integrations/tls/mbedtls/tls_mbedtls.o"

GATE_INCLUDES="-Ivendor/keel/include -Ivendor/keel/integrations/tls/mbedtls"
CFLAGS="-std=c11 -Wall -Wextra -g"

# mbedTLS compile flags (mirrors mk/vendor/mbedtls.mk): the config file lives at
# vendor/mbedtls/hull_config.h, reached via -I$(MBEDTLS_DIR). The includes are
# kept word-splittable; the -DMBEDTLS_CONFIG_FILE='"..."' define is passed as a
# LITERAL arg at each call site (never through a variable + eval, which would
# strip the inner quotes and expand the config-file name unquoted).
MBEDTLS_INCS="-std=c11 -O2 -w -I$MBEDTLS_DIR/include -I$MBEDTLS_DIR/library -I$MBEDTLS_DIR"

# Compile one mbedTLS library source to an object. $1 = compiler, $2 = src, $3 = obj.
compile_mbedtls_obj() {
    # shellcheck disable=SC2086
    "$1" $MBEDTLS_INCS -DMBEDTLS_CONFIG_FILE='"hull_config.h"' -c -o "$3" "$2"
}

# Report the backend baked into the built libkeel.a.
report_backend() {
    _b=$(ar t "$KEEL_LIB" 2>/dev/null | grep -oE 'event_(epoll|kqueue|poll)\.o' | head -1 | sed -E 's/event_(.*)\.o/\1/')
    [ -n "$_b" ] && echo "$_b" || echo "unknown"
}

# ── (0) TLS material: self-signed server cert (CN/SAN = spike.local) + an
#        unrelated CA for the unknown-CA negative case. Generated once (host
#        openssl). The APE, like the native binary, just READS the .pem files. ──
gen_tls_material() {
    if [ ! -f spike/tls/server.crt ] || [ ! -f spike/tls/server.key ] || [ ! -f spike/tls/other.crt ]; then
        echo "==> generating hermetic TLS material under spike/tls/ (host openssl) ..."
        if ! command -v openssl >/dev/null 2>&1; then
            echo "==> ERROR: openssl not found on PATH (needed to generate spike/tls/*)." >&2
            exit 1
        fi
        mkdir -p spike/tls
        openssl req -x509 -newkey rsa:2048 -nodes -keyout spike/tls/server.key \
            -out spike/tls/server.crt -days 3650 \
            -subj "/CN=spike.local/O=hull-spike" \
            -addext "subjectAltName=DNS:spike.local" >/dev/null 2>&1
        openssl req -x509 -newkey rsa:2048 -nodes -keyout spike/tls/other.key \
            -out spike/tls/other.crt -days 3650 \
            -subj "/CN=other-ca.example/O=other" \
            -addext "subjectAltName=DNS:other-ca.example" >/dev/null 2>&1
    else
        echo "==> TLS material present under spike/tls/"
    fi
}

# =============================================================================
# COSMO MODE
# =============================================================================
if [ "$COSMO" -eq 1 ]; then
    echo "==> spike/build.sh --cosmo: Cosmopolitan APE (poll readiness)"
    if ! command -v cosmocc >/dev/null 2>&1; then
        echo "==> ERROR: cosmocc not found on PATH." >&2
        echo "    Install it (as Hull's CI does):" >&2
        echo "      COSMOCC_DIR=/opt/cosmo make fetch-cosmocc && export PATH=/opt/cosmo/bin:\$PATH" >&2
        exit 1
    fi
    CC=cosmocc

    gen_tls_material

    OUT_DIR="$ROOT/build/spike-cosmo"
    MBED_OUT="$OUT_DIR/mbed"
    mkdir -p "$MBED_OUT"

    # ── build libkeel.a under cosmocc (auto-selects poll) ─────────────────────
    echo "==> building libkeel.a with CC=cosmocc (auto poll backend) ..."
    make -C "$KEEL_DIR" clean >/dev/null 2>&1 || true
    make -C "$KEEL_DIR" CC=cosmocc AR=ar \
        KEEL_TLS=mbedtls MBEDTLS_DIR="$ROOT/$MBEDTLS_DIR" MBEDTLS_CONFIG_FILE=hull_config.h \
        KEEL_COMPRESS=miniz MINIZ_DIR="$ROOT/$MINIZ_DIR"
    echo "==> libkeel.a backend (cosmo): $(report_backend)"

    # ── build mbedTLS objects under cosmocc (the staged build/mbed_*.o are the
    #    HOST arch; cosmo needs its own). Compile the whole library/*.c set. ────
    echo "==> compiling mbedTLS objects with cosmocc into $MBED_OUT/ ..."
    MBED_OBJS=""
    for src in "$MBEDTLS_DIR"/library/*.c; do
        base=$(basename "$src" .c)
        obj="$MBED_OUT/mbed_$base.o"
        if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
            compile_mbedtls_obj cosmocc "$src" "$obj"
        fi
        MBED_OBJS="$MBED_OBJS $obj"
    done
    echo "==> built $(echo "$MBED_OBJS" | wc -w | tr -d ' ') cosmo mbedTLS objects"

    # ── compile + link + run the spike (poll readiness) ───────────────────────
    OBJ="$OUT_DIR/smtp_keel_spike.o"
    BIN="$OUT_DIR/smtp_keel_spike"
    echo
    echo "==> COMPILE GATE (public headers only) [cosmo poll]:"
    echo "    cosmocc $CFLAGS $GATE_INCLUDES -c spike/smtp_keel_spike.c -o $OBJ"
    # shellcheck disable=SC2086
    cosmocc $CFLAGS $GATE_INCLUDES -c spike/smtp_keel_spike.c -o "$OBJ"

    echo "==> LINK [cosmo poll]:"
    # shellcheck disable=SC2086
    cosmocc $CFLAGS "$OBJ" "$TLS_INT_OBJ" "$KEEL_LIB" $MBED_OBJS -lm -o "$BIN"

    echo "==> RUN [cosmo poll]: $BIN"
    RC=0
    if "$BIN"; then
        echo "==> RESULT [cosmo poll]: PASS"
    else
        echo "==> RESULT [cosmo poll]: FAIL"
        RC=1
    fi

    echo
    if [ "$RC" -eq 0 ]; then
        echo "########################################"
        echo "# spike/build.sh --cosmo: OVERALL PASS (Cosmopolitan APE, poll readiness)"
        echo "########################################"
    else
        echo "########################################"
        echo "# spike/build.sh --cosmo: OVERALL FAIL"
        echo "########################################"
    fi
    exit "$RC"
fi

# =============================================================================
# NATIVE MODE (Linux epoll / macOS kqueue, plus explicit poll) - default
# =============================================================================
OUT_DIR="$ROOT/build/spike"
mkdir -p "$OUT_DIR"

gen_tls_material

# ── mbedTLS objects: prefer build/mbed_*.o (Hull's staged objects) ─────────────
MBED_OBJS=$(ls "$ROOT"/build/mbed_*.o 2>/dev/null || true)
if [ -n "$MBED_OBJS" ]; then
    echo "==> using $(echo "$MBED_OBJS" | wc -w | tr -d ' ') staged mbedTLS objects from build/"
else
    echo "==> no staged build/mbed_*.o; compiling mbedTLS objects with $CC into $OUT_DIR/mbed/ ..."
    MBED_OUT="$OUT_DIR/mbed"
    mkdir -p "$MBED_OUT"
    MBED_OBJS=""
    for src in "$MBEDTLS_DIR"/library/*.c; do
        base=$(basename "$src" .c)
        obj="$MBED_OUT/mbed_$base.o"
        if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
            compile_mbedtls_obj "$CC" "$src" "$obj"
        fi
        MBED_OBJS="$MBED_OBJS $obj"
    done
    echo "==> built $(echo "$MBED_OBJS" | wc -w | tr -d ' ') mbedTLS objects"
fi

# Build libkeel.a for a given backend. $1 = "" (platform default) or "poll".
build_keel() {
    _bk="$1"
    make -C "$KEEL_DIR" clean >/dev/null 2>&1 || true
    if [ -z "$_bk" ]; then
        echo "==> building libkeel.a (platform default backend) ..."
        make -C "$KEEL_DIR" CC="$CC" AR="$AR" \
            KEEL_TLS=mbedtls MBEDTLS_DIR="$ROOT/$MBEDTLS_DIR" MBEDTLS_CONFIG_FILE=hull_config.h \
            KEEL_COMPRESS=miniz MINIZ_DIR="$ROOT/$MINIZ_DIR" >/dev/null
    else
        echo "==> building libkeel.a (BACKEND=$_bk) ..."
        make -C "$KEEL_DIR" CC="$CC" AR="$AR" BACKEND="$_bk" \
            KEEL_TLS=mbedtls MBEDTLS_DIR="$ROOT/$MBEDTLS_DIR" MBEDTLS_CONFIG_FILE=hull_config.h \
            KEEL_COMPRESS=miniz MINIZ_DIR="$ROOT/$MINIZ_DIR" >/dev/null
    fi
    echo "==> libkeel.a backend: $(report_backend)"
}

# Compile + link + run one variant. $1 = extra flags, $2 = suffix, $3 = label.
compile_run() {
    EXTRA="$1"; SUFFIX="$2"; LABEL="$3"
    OBJ="$OUT_DIR/smtp_keel_spike$SUFFIX.o"
    BIN="$OUT_DIR/smtp_keel_spike$SUFFIX"

    echo
    echo "==> COMPILE GATE (public headers only) [$LABEL]:"
    echo "    $CC $CFLAGS $EXTRA $GATE_INCLUDES -c spike/smtp_keel_spike.c -o $OBJ"
    # shellcheck disable=SC2086
    $CC $CFLAGS $EXTRA $GATE_INCLUDES -c spike/smtp_keel_spike.c -o "$OBJ"

    echo "==> LINK [$LABEL]:"
    # shellcheck disable=SC2086
    $CC $CFLAGS $EXTRA "$OBJ" "$TLS_INT_OBJ" "$KEEL_LIB" $MBED_OBJS -lm -lpthread -o "$BIN"

    echo "==> RUN [$LABEL]: $BIN"
    if "$BIN"; then
        echo "==> RESULT [$LABEL]: PASS"
        return 0
    else
        echo "==> RESULT [$LABEL]: FAIL"
        return 1
    fi
}

RC=0

# ── (A) platform default backend (epoll on Linux / kqueue on macOS) ───────────
build_keel ""
DEFAULT_BK=$(report_backend)
echo "==> platform default readiness backend detected: $DEFAULT_BK"
compile_run ""                    ""            "$DEFAULT_BK plain"          || RC=1
if compile_run "-fsanitize=address" "_asan"     "$DEFAULT_BK ASan"; then
    echo "==> $DEFAULT_BK ASan: PASS (no UAF/overflow; heap-leak backstop is Linux CI LSan)"
else
    echo "==> $DEFAULT_BK ASan: FAIL or unavailable (plain result above is authoritative)"
    RC=1
fi

# ── (B) poll backend: rebuild libkeel, plain + ASan ───────────────────────────
build_keel poll
compile_run ""                    "_poll"       "poll plain"            || RC=1
if compile_run "-fsanitize=address" "_poll_asan" "poll ASan"; then
    echo "==> poll ASan: PASS"
else
    echo "==> poll ASan: FAIL or unavailable"
    RC=1
fi

# ── (C) RESTORE the platform default backend so the tree is left as found ─────
echo
echo "==> restoring the platform default backend for vendor/keel/libkeel.a ..."
build_keel ""

echo
if [ "$RC" -eq 0 ]; then
    echo "########################################"
    echo "# spike/build.sh: OVERALL PASS ($DEFAULT_BK + poll, plain + ASan)"
    echo "########################################"
else
    echo "########################################"
    echo "# spike/build.sh: OVERALL FAIL"
    echo "########################################"
fi
exit "$RC"
