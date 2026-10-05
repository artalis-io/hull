;; loop_forever.wat - a hull_process that never returns (67 bytes as .wasm)
;;
;; Only the wall-clock watchdog can stop it (gas aside): used to check that the
;; manifest / CLI timeout ceilings reach every entry point (round-6 M3).
;; Hand-assembled; the bytes match loop_forever_wasm[] in tests/hull/cap/test_wasm.c.

(module
  (memory (export "memory") 1)
  (func (export "hull_process")
    (param i32 i32 i32 i32) (result i32)
    (loop (br 0))
    unreachable))
