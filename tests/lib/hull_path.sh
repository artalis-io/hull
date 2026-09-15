# hull_path.sh - compare a path Hull PRINTED against one this shell BUILT.
#
# WHY THIS EXISTS. A POSIX path handed to an APE does not come back spelled the
# way the shell spells it. The MSYS shell converts "/d/a/x" to the Win32 mixed
# form "D:/a/x" on the way in - with the drive letter in its canonical
# UPPERCASE - and hl_host_normalize_path turns that back into "/D/a/x",
# preserving the case it was given. Neither side is wrong and Hull is behaving
# correctly; the shell's own variable simply holds the pre-conversion spelling.
#
# So a check written as `case "$OUT" in *"$DIR"*)` fails on a one-character
# difference in the drive letter. Measured on a Windows runner:
#
#   shell's $ISOLATED      /d/a/_temp/tmp.ZIqE9qR5sr
#   hull's own output      /D/a/_temp/tmp.ZIqE9qR5sr
#
# WHAT THIS DOES NOT DO. It does not normalise both sides into agreement - that
# would let a genuinely wrong path pass. It tries the needle VERBATIM first, so
# on every host the original exact comparison is what decides; only if that
# fails, and only where an APE is actually involved, does it retry with the
# drive-letter segment upper-cased. A lowercase /d/... directory on Linux is
# therefore never touched, and a path wrong in any other way still fails.
#
# Requires hull_rc.sh to have been sourced first (HULL_RC_SHIFT is its APE
# probe). Without it this degrades to exact matching, which is the strict side.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

# Upper-case a leading "/<single letter>/" drive segment; leave anything else
# alone. tr, not a GNU-only sed \U, so macOS is fine too.
hull_path__canon() {
    case "$1" in
        /?/*)
            _d=$(printf '%s' "$1" | cut -c2 | tr 'a-z' 'A-Z')
            printf '/%s%s' "$_d" "$(printf '%s' "$1" | cut -c3-)"
            ;;
        *) printf '%s' "$1" ;;
    esac
}

# hull_path_alt NEEDLE -> the drive-canonical spelling of NEEDLE on an APE
# host, and NEEDLE unchanged everywhere else - so using it as an extra `case`
# alternative is a harmless duplicate off Windows rather than a second, looser
# pattern. Use this where the comparison is compound (a literal AND a path, in
# order) and hull_path_contains cannot express it.
hull_path_alt() {
    [ "${HULL_RC_SHIFT:-0}" = 1 ] || { printf '%s' "$1"; return 0; }
    hull_path__canon "$1"
}

# hull_path_contains HAYSTACK NEEDLE -> 0 if HAYSTACK contains NEEDLE, trying
# the verbatim spelling first and the drive-canonical one only on an APE host.
hull_path_contains() {
    case "$1" in *"$2"*) return 0 ;; esac
    _c=$(hull_path_alt "$2")
    [ "$_c" = "$2" ] && return 1
    case "$1" in *"$_c"*) return 0 ;; esac
    return 1
}
