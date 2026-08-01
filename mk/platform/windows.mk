# mk/platform/windows.mk - native Windows platform policy (STUB / forward seam).
#
# Hull runs on Windows TODAY via the Cosmopolitan APE (`hull-cosmo`): one fat
# binary, no native Windows toolchain, no code here. This file documents the
# seam a future NATIVE Windows port (MSVC / clang-cl) would fill, so the platform
# axis (mk/platform/{darwin,linux,cosmo,windows}.mk) is complete by construction.
#
# A native port fills in:
#   - Sandbox backend: Restricted Tokens / AppContainer / Job Objects, behind the
#     same hl_sandbox_apply seam Linux pledge/unveil and macOS seatbelt use.
#   - Sealed arena (docs/security.md 5b): VirtualAlloc + VirtualProtect in place
#     of mmap/mprotect (the hl_seal_arena _WIN32 branch noted in the c-audit skill).
#   - Link flags: MSVC/clang-cl equivalents of the -Wl,-z,* hardening + the OS
#     libs (ws2_32, bcrypt, ...), and the deterministic-archive envelope.
#   - CA trust: the embedded Mozilla bundle already covers this (no system store
#     dependency), so no work beyond linking it.
#
# Until then this file is intentionally empty: `include mk/platform/windows.mk`
# on a hypothetical native-Windows build is a no-op that fails loudly only if the
# port forgets to fill it in.
