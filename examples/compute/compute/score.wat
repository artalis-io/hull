;; score.wat — Simple scoring compute module
;;
;; Computes a score (0-100) from input bytes by summing them mod 101.
;; Demonstrates a pure WASM compute plugin for Hull.
;;
;; Compile: wat2wasm score.wat -o score.wasm

(module
  (import "env" "host_call" (func $host_call (param i32 i32 i32) (result i32)))
  (memory (export "memory") 1)

  (func $hull_process (export "hull_process")
    (param $in_ptr i32) (param $in_len i32)
    (param $out_ptr i32) (param $out_max i32)
    (result i32)
    (local $i i32)
    (local $sum i32)

    ;; Need at least 1 byte of output
    (if (i32.lt_u (local.get $out_max) (i32.const 1))
      (then (return (i32.const -2)))
    )

    ;; Sum all input bytes
    (local.set $sum (i32.const 0))
    (local.set $i (i32.const 0))
    (block $break
      (loop $loop
        (br_if $break (i32.ge_u (local.get $i) (local.get $in_len)))
        (local.set $sum
          (i32.add
            (local.get $sum)
            (i32.load8_u (i32.add (local.get $in_ptr) (local.get $i)))
          )
        )
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $loop)
      )
    )

    ;; score = sum % 101 (range 0-100)
    (i32.store8
      (local.get $out_ptr)
      (i32.rem_u (local.get $sum) (i32.const 101))
    )

    ;; Return 1 byte written
    (i32.const 1)
  )

  (func $hull_version (export "hull_version") (result i32)
    (i32.const 1)
  )
)
