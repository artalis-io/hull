;; oob.wat - a hull_process that loads or stores outside its linear memory
;; (audit 31). The first input byte picks the operation:
;;   'r'  i32.load  at 0x40000000 (1 GiB, far past the memory) -> trap
;;   'w'  i32.store at 0x40000000                             -> trap
;;   else write "k" to the output and return 1 (the module works)
;;
;; Hull's WAMR runtime maps linear memory at its exact size, with no guard
;; region, so only a software bounds check stops these accesses in AOT code:
;; tests/e2e_compute_aot_bounds.sh AOT-compiles this module through `hull build`
;; and asserts both calls trap instead of touching host memory.
;; Regenerate: wat2wasm tests/fixtures/compute/oob.wat -o tests/fixtures/compute/oob.wasm

(module
  (memory (export "memory") 1)
  (func (export "hull_process")
    (param $in_ptr i32) (param $in_len i32)
    (param $out_ptr i32) (param $out_max i32)
    (result i32)
    (local $op i32)
    (if (i32.lt_u (local.get $out_max) (i32.const 4))
      (then (return (i32.const -2))))
    (local.set $op
      (if (result i32) (i32.eqz (local.get $in_len))
        (then (i32.const 0))
        (else (i32.load8_u (local.get $in_ptr)))))
    (if (i32.eq (local.get $op) (i32.const 0x72))     ;; 'r'
      (then
        (i32.store (local.get $out_ptr)
                   (i32.load (i32.const 0x40000000)))
        (return (i32.const 4))))
    (if (i32.eq (local.get $op) (i32.const 0x77))     ;; 'w'
      (then
        (i32.store (i32.const 0x40000000) (i32.const 0x41414141))
        (return (i32.const 0))))
    (i32.store8 (local.get $out_ptr) (i32.const 0x6b)) ;; 'k'
    (i32.const 1)))
