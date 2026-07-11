//! embed_rust — reference Rust host for the libhull no-runtime flavor.
//!
//! Links only libhull.a + Keel (see build.rs); no Lua/QuickJS runtime. Drives
//! the runtime-free Hull core through the stable C ABI in <hull/embed.h> via a
//! small `extern "C"` block — the analogue of examples/embed_c, from Rust.
//! Exits non-zero on any capability failure.
//!
//! SPDX-License-Identifier: AGPL-3.0-or-later

use std::cell::Cell;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::process::exit;
use std::ptr;

#[repr(C)]
struct HlEmbed {
    _private: [u8; 0],
}

extern "C" {
    fn hl_embed_abi_version() -> c_int;
    fn hl_embed_new(app_dir: *const c_char) -> *mut HlEmbed;
    fn hl_embed_free(e: *mut HlEmbed);
    fn hl_embed_sandbox_phase1(e: *mut HlEmbed) -> c_int;
    fn hl_embed_allow_read(e: *mut HlEmbed, rel_path: *const c_char) -> c_int;
    fn hl_embed_allow_write(e: *mut HlEmbed, rel_path: *const c_char) -> c_int;
    fn hl_embed_allow_network(e: *mut HlEmbed, inbound: c_int, outbound: c_int);
    fn hl_embed_seal(e: *mut HlEmbed, db_path: *const c_char) -> c_int;
    fn hl_embed_fs_write(
        e: *mut HlEmbed,
        path: *const c_char,
        data: *const c_void,
        len: usize,
        err: *mut *const c_char,
    ) -> c_int;
    fn hl_embed_fs_read(
        e: *mut HlEmbed,
        path: *const c_char,
        buf: *mut c_char,
        buf_size: usize,
        err: *mut *const c_char,
    ) -> i64;
    fn hl_embed_fs_exists(e: *mut HlEmbed, path: *const c_char, err: *mut *const c_char) -> c_int;
    fn hl_embed_fs_delete(e: *mut HlEmbed, path: *const c_char, err: *mut *const c_char) -> c_int;
    fn hl_embed_sha256(data: *const c_void, len: usize, out: *mut u8) -> c_int;
    fn hl_embed_platform() -> *const c_char;
    fn hl_embed_module_count() -> usize;

    fn chdir(path: *const c_char) -> c_int;
}

fn cstr(s: &str) -> CString {
    CString::new(s).unwrap()
}

fn main() {
    let failures = Cell::new(0u32);
    let check = |ok: bool, what: &str| {
        println!("  [{}] {}", if ok { "PASS" } else { "FAIL" }, what);
        if !ok {
            failures.set(failures.get() + 1);
        }
    };

    unsafe {
        println!(
            "embed_rust: libhull no-runtime host (ABI v{})",
            hl_embed_abi_version()
        );

        // A native host owns its working directory. Absolute temp dir.
        let dir = std::env::temp_dir().join(format!("hull-embed-rust-{}", std::process::id()));
        std::fs::create_dir_all(&dir).expect("create temp dir");
        let dir_c = cstr(dir.to_str().expect("utf-8 path"));
        assert_eq!(chdir(dir_c.as_ptr()), 0, "chdir into temp dir");

        // 1. handle
        let e = hl_embed_new(dir_c.as_ptr());
        check(!e.is_null(), "hl_embed_new(app_dir)");
        if e.is_null() {
            exit(1);
        }

        // 2. phase-1 sandbox
        check(hl_embed_sandbox_phase1(e) == 0, "phase-1 sandbox applied");

        // 3. policy (app_dir-relative, like a manifest)
        let dot = cstr(".");
        check(hl_embed_allow_read(e, dot.as_ptr()) == 0, "allow_read(\".\")");
        check(hl_embed_allow_write(e, dot.as_ptr()) == 0, "allow_write(\".\")");
        hl_embed_allow_network(e, 0, 0);

        // fail-closed before seal
        let note = cstr("note.txt");
        let mut pre_err: *const c_char = ptr::null();
        check(
            hl_embed_fs_exists(e, note.as_ptr(), &mut pre_err) == -1 && !pre_err.is_null(),
            "capabilities fail closed before seal",
        );

        // 4. seal (phase-2 sandbox) — must check
        let sealed = hl_embed_seal(e, ptr::null());
        check(sealed == 0, "hl_embed_seal applied sandbox");
        if sealed != 0 {
            hl_embed_free(e);
            exit(1);
        }

        // 5. capability-mediated fs I/O
        let mut err: *const c_char = ptr::null();
        let payload = b"hull embed_rust capability write\n";
        let w = hl_embed_fs_write(
            e,
            note.as_ptr(),
            payload.as_ptr() as *const c_void,
            payload.len(),
            &mut err,
        );
        check(w == 0, "hl_embed_fs_write under sandbox");

        let mut buf = [0 as c_char; 128];
        let n = hl_embed_fs_read(e, note.as_ptr(), buf.as_mut_ptr(), buf.len(), &mut err);
        let round_ok = n == payload.len() as i64 && {
            let read = std::slice::from_raw_parts(buf.as_ptr() as *const u8, n as usize);
            read == &payload[..]
        };
        check(round_ok, "hl_embed_fs_read round-trips");

        let esc = cstr("../escape");
        check(
            hl_embed_fs_exists(e, esc.as_ptr(), &mut err) == -1,
            "hl_embed_fs rejects path traversal",
        );

        // 6. crypto (stateless)
        let mut digest = [0u8; 32];
        let abc = b"abc";
        let cc = hl_embed_sha256(abc.as_ptr() as *const c_void, 3, digest.as_mut_ptr());
        check(
            cc == 0 && digest[0] == 0xba && digest[1] == 0x78 && digest[2] == 0x16 && digest[3] == 0xbf,
            "hl_embed_sha256(\"abc\") matches known vector",
        );

        // 7. identity
        let plat = hl_embed_platform();
        check(!plat.is_null() && *plat != 0, "hl_embed_platform reports arch");
        check(hl_embed_module_count() > 0, "hl_embed_module_count populated");
        let plat_str = if plat.is_null() {
            "(null)".to_string()
        } else {
            CStr::from_ptr(plat).to_string_lossy().into_owned()
        };
        println!("  platform={} modules={}", plat_str, hl_embed_module_count());

        hl_embed_fs_delete(e, note.as_ptr(), &mut err);
        hl_embed_free(e);
        let _ = std::fs::remove_dir_all(&dir);
    }

    let f = failures.get();
    println!(
        "embed_rust: {} ({} failure{})",
        if f == 0 { "OK" } else { "FAILED" },
        f,
        if f == 1 { "" } else { "s" }
    );
    exit(if f == 0 { 0 } else { 1 });
}
