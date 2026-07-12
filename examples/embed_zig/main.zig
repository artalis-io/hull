//! embed_zig — reference Zig host for the libhull no-runtime flavor.
//!
//! Zig @cImports include/hull/embed.h DIRECTLY (no hand-written bindings) and
//! links libhull.a + Keel; no Lua/QuickJS runtime. Drives the runtime-free
//! Hull core through the stable C ABI — the analogue of examples/embed_c, from
//! Zig. Because @cImport consumes the header as-is, a clean compile is itself
//! evidence the ABI header is FFI-consumable. Exits non-zero on any failure.
//!
//! Built with Zig 0.13.0. SPDX-License-Identifier: AGPL-3.0-or-later

const std = @import("std");
const c = @cImport({
    @cInclude("hull/embed.h");
    @cInclude("stdlib.h"); // mkdtemp
    @cInclude("unistd.h"); // chdir, rmdir
});

var failures: u32 = 0;

fn check(ok: bool, what: []const u8) void {
    std.debug.print("  [{s}] {s}\n", .{ if (ok) "PASS" else "FAIL", what });
    if (!ok) failures += 1;
}

pub fn main() u8 {
    std.debug.print(
        "embed_zig: libhull no-runtime host (ABI v{d})\n",
        .{c.hl_embed_abi_version()},
    );

    // A native host owns its working directory. Fresh absolute temp dir.
    var tmpl: [64]u8 = undefined;
    const path = std.fmt.bufPrintZ(&tmpl, "/tmp/hull-embed-zig.XXXXXX", .{}) catch return 1;
    const dir = c.mkdtemp(path.ptr);
    if (dir == null) return 1;
    if (c.chdir(dir) != 0) return 1;

    // 1. handle
    const e = c.hl_embed_new(dir);
    check(e != null, "hl_embed_new(app_dir)");
    if (e == null) return 1;

    // 2. phase-1 sandbox
    check(c.hl_embed_sandbox_phase1(e) == 0, "phase-1 sandbox applied");

    // 3. policy (app_dir-relative, like a manifest)
    check(c.hl_embed_allow_read(e, ".") == 0, "allow_read(\".\")");
    check(c.hl_embed_allow_write(e, ".") == 0, "allow_write(\".\")");
    c.hl_embed_allow_network(e, 0, 0);

    // fail-closed before seal
    var pre_err: [*c]const u8 = null;
    check(
        c.hl_embed_fs_exists(e, "note.txt", &pre_err) == -1 and pre_err != null,
        "capabilities fail closed before seal",
    );

    // 4. seal (phase-2 sandbox) — must check
    const sealed = c.hl_embed_seal(e, null);
    check(sealed == 0, "hl_embed_seal applied sandbox");
    if (sealed != 0) {
        c.hl_embed_free(e);
        return 1;
    }

    // 5. capability-mediated fs I/O
    var err: [*c]const u8 = null;
    const payload = "hull embed_zig capability write\n";
    const w = c.hl_embed_fs_write(e, "note.txt", payload, payload.len, &err);
    check(w == 0, "hl_embed_fs_write under sandbox");

    var buf: [128]u8 = undefined;
    const n = c.hl_embed_fs_read(e, "note.txt", &buf, buf.len, &err);
    const round_ok = n == @as(i64, @intCast(payload.len)) and
        std.mem.eql(u8, buf[0..@intCast(n)], payload[0..payload.len]);
    check(round_ok, "hl_embed_fs_read round-trips");

    check(c.hl_embed_fs_exists(e, "../escape", &err) == -1, "hl_embed_fs rejects path traversal");

    // 6. crypto (stateless)
    var digest: [32]u8 = undefined;
    const cc = c.hl_embed_sha256("abc", 3, &digest);
    check(
        cc == 0 and digest[0] == 0xba and digest[1] == 0x78 and digest[2] == 0x16 and digest[3] == 0xbf,
        "hl_embed_sha256(\"abc\") matches known vector",
    );

    // 7. identity
    const plat = c.hl_embed_platform();
    check(plat != null and plat[0] != 0, "hl_embed_platform reports arch");
    check(c.hl_embed_module_count() > 0, "hl_embed_module_count populated");
    if (plat != null) {
        std.debug.print(
            "  platform={s} modules={d}\n",
            .{ std.mem.span(plat), c.hl_embed_module_count() },
        );
    } else {
        std.debug.print("  platform=(null) modules={d}\n", .{c.hl_embed_module_count()});
    }

    _ = c.hl_embed_fs_delete(e, "note.txt", &err);
    c.hl_embed_free(e);
    _ = c.rmdir(dir);

    std.debug.print(
        "embed_zig: {s} ({d} failure{s})\n",
        .{ if (failures == 0) "OK" else "FAILED", failures, if (failures == 1) "" else "s" },
    );
    return if (failures == 0) 0 else 1;
}
