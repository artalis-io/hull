;; spin.wat — Deliberately slow WASM module for concurrency testing
;;
;; Burns CPU by looping (in_len * 10000) iterations of integer arithmetic.
;; With fast-interp: ~500ms for 1KB input, ~50ms for 100B input.
;; Returns a single byte (the final accumulator value mod 256).

(module
  (import "env" "host_call" (func $host_call (param i32 i32 i32) (result i32)))
  (memory (export "memory") 1)

  (func $hull_process (export "hull_process")
    (param $in_ptr i32) (param $in_len i32)
    (param $out_ptr i32) (param $out_max i32)
    (result i32)

    (local $i i32) (local $limit i32) (local $acc i32)

    (if (i32.lt_u (local.get $out_max) (i32.const 1))
      (then (return (i32.const -2)))
    )

    ;; limit = in_len * 100
    (local.set $limit (i32.mul (local.get $in_len) (i32.const 100)))
    (local.set $acc (i32.const 1))
    (local.set $i (i32.const 0))

    (block $done
      (loop $spin
        (br_if $done (i32.ge_u (local.get $i) (local.get $limit)))

        ;; Busy work: acc = (acc * 1103515245 + 12345) — LCG step
        (local.set $acc
          (i32.add
            (i32.mul (local.get $acc) (i32.const 1103515245))
            (i32.const 12345)
          )
        )

        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $spin)
      )
    )

    ;; Write result byte
    (i32.store8 (local.get $out_ptr) (local.get $acc))
    (i32.const 1)
  )

  (func $hull_version (export "hull_version") (result i32)
    (i32.const 1)
  )
)
