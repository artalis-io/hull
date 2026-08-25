// hull:_probe_util — a trivial tooling helper, imported by hull:probe to prove that the
// restricted QuickJS tooling runtime can load a MULTI-MODULE bundle from the cli-js VFS.
// Trusted, bundled, tooling-only; never reachable by application code.
// SPDX-License-Identifier: AGPL-3.0-or-later
export function marker() {
    return "probe-util-ok";
}
