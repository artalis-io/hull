;; echo.wat — Test WASM module for Hull compute capability
;;
;; Exports:
;;   hull_process(in_ptr, in_len, out_ptr, out_max) -> bytes_written
;;   hull_version() -> 1
;;
;; Behavior: copies input to output (echo). Returns -2 if output too small.

(module
  ;; Import host_call from env (optional — not used by echo)
  (import "env" "host_call" (func $host_call (param i32 i32 i32) (result i32)))

  ;; Memory: 1 page (64KB) — host manages allocation via module_malloc
  (memory (export "memory") 1)

  ;; hull_process: echo input to output
  (func $hull_process (export "hull_process")
    (param $in_ptr i32) (param $in_len i32)
    (param $out_ptr i32) (param $out_max i32)
    (result i32)

    ;; if in_len > out_max, return -2 (output too small)
    (if (i32.gt_u (local.get $in_len) (local.get $out_max))
      (then (return (i32.const -2)))
    )

    ;; memcpy(out_ptr, in_ptr, in_len) using memory.copy (bulk memory)
    (memory.copy
      (local.get $out_ptr)
      (local.get $in_ptr)
      (local.get $in_len)
    )

    ;; return in_len (bytes written)
    (local.get $in_len)
  )

  ;; hull_version: return ABI version 1
  (func $hull_version (export "hull_version") (result i32)
    (i32.const 1)
  )
)
