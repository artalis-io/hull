//! Link the reference Rust host against the runtime-free Hull core.
//!
//! libhull.a and Keel are mutually dependent (libhull's capability layer
//! calls Keel's TLS, which calls libhull's mbedTLS), and GNU ld / lld resolve
//! static archives strictly left-to-right, so libhull.a is passed twice:
//! `libhull.a libkeel.a libhull.a`. We pass the archives as positional link
//! args (not `-l`, which cargo would dedup) to preserve that order.
//!
//! Paths default to the in-tree build (../../build/libhull.a,
//! ../../vendor/keel/libkeel.a) and can be overridden with the env vars
//! HULL_LIBHULL_A and HULL_LIBKEEL_A (the `make embed-rust-smoke` target sets
//! them to absolute paths).

use std::env;
use std::path::PathBuf;

fn resolve(var: &str, default_rel: &str) -> PathBuf {
    if let Ok(p) = env::var(var) {
        return PathBuf::from(p);
    }
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    manifest.join(default_rel)
}

fn main() {
    let libhull = resolve("HULL_LIBHULL_A", "../../build/libhull.a");
    let libkeel = resolve("HULL_LIBKEEL_A", "../../vendor/keel/libkeel.a");

    // Absolute paths so the linker (run from the target dir) finds them.
    let libhull = libhull.canonicalize().unwrap_or(libhull);
    let libkeel = libkeel.canonicalize().unwrap_or(libkeel);

    // Order matters: libhull -> keel -> libhull resolves the archive cycle.
    println!("cargo:rustc-link-arg={}", libhull.display());
    println!("cargo:rustc-link-arg={}", libkeel.display());
    println!("cargo:rustc-link-arg={}", libhull.display());
    // mbedTLS/WAMR need libm; pthread comes in via Rust's std on all targets.
    println!("cargo:rustc-link-lib=m");

    println!("cargo:rerun-if-changed={}", libhull.display());
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=HULL_LIBHULL_A");
    println!("cargo:rerun-if-env-changed=HULL_LIBKEEL_A");
}
