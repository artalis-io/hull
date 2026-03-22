;; echo64.wat — Memory64 echo module (copies input to output)
;;
;; Same as echo.wasm but uses 64-bit memory addressing.
;; Must be AOT-compiled to run (fast interpreter lacks Memory64 support).

(module
  (import "env" "host_call" (func $host_call (param i32 i32 i32) (result i32)))

  (memory (export "memory") i64 1)

  (func $hull_process (export "hull_process")
    (param $in_ptr i64) (param $in_len i64)
    (param $out_ptr i64) (param $out_max i64)
    (result i32)

    ;; If in_len > out_max, return -2
    (if (i64.gt_u (local.get $in_len) (local.get $out_max))
      (then (return (i32.const -2)))
    )

    ;; memory.copy dst=out_ptr src=in_ptr len=in_len
    (memory.copy
      (local.get $out_ptr)
      (local.get $in_ptr)
      (local.get $in_len)
    )

    ;; Return bytes written (truncated to i32 — fine for reasonable sizes)
    (i32.wrap_i64 (local.get $in_len))
  )

  (func $hull_version (export "hull_version") (result i32)
    (i32.const 1)
  )
)
