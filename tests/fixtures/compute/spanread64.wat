;; spanread64.wat — Memory64 mapped-span reader (#334).
;;
;; A (memory i64) guest that exercises the SPAN_INFO metadata host-call under
;; Memory64 AOT: it queries the span count, decodes span 0's record, and reads
;; window[0] through the record's 64-bit `base` (a span whose window sits above
;; UINT32_MAX under mem64). Uses the RAW host_call(0x04, ...) ABI (no hull_span.h,
;; so no freestanding memcpy needed), mirroring spancount.c.
;;
;; Scratch: the host-allocated INPUT buffer (in_ptr) is reused as the 96-byte
;; SPAN_INFO scratch record -- host-allocated => valid, sub-4 GiB, and guaranteed
;; not to collide with the output buffer. The test passes an input >= 96 bytes.
;;
;; Output (12 bytes): [0]=span count, [1]=window[0] via 64-bit base,
;;   [2]=(0xfffffff0 scratch was rejected -1 ? 1 : 0), [3..10]=base (u64 LE),
;;   [11]=(record query returned struct_size 96 ? 1 : 0).
;;
;; Memory64 requires AOT (the fast interpreter cannot load it).
;; Build (byte-reproducible): tests/fixtures/compute/build_spanread64.sh
;;   (wat2wasm --enable-memory64; SHA-256 pinned there).

(module
  (import "env" "host_call" (func $host_call (param i32 i32 i32) (result i32)))

  (memory (export "memory") i64 1)

  (func $hull_process (export "hull_process")
    (param $in_ptr i64) (param $in_len i64)
    (param $out_ptr i64) (param $out_max i64)
    (result i32)

    (local $count i32)
    (local $reject i32)
    (local $r i32)
    (local $base i64)
    (local $scratch32 i32)

    ;; need 12 output bytes and a >= 96-byte input to use as scratch
    (if (i64.lt_u (local.get $out_max) (i64.const 12))
      (then (return (i32.const -3))))          ;; HULL_ERR_OUTPUT
    (if (i64.lt_u (local.get $in_len) (i64.const 96))
      (then (return (i32.const -1))))

    ;; 1. count query: host_call(0x04, 0, -1) -- no scratch written
    (local.set $count
      (call $host_call (i32.const 0x04) (i32.const 0) (i32.const -1)))

    ;; 2. bogus in-i32-range scratch -> host validates vs linear memory -> -1
    (local.set $reject
      (call $host_call (i32.const 0x04) (i32.const 0xfffffff0) (i32.const 0)))

    ;; 3. cbSize handshake: advertise struct_size = 96 at scratch+2, then query
    ;;    record 0. Scratch = the host-allocated input buffer (in_ptr), narrowed
    ;;    to i32 for the fixed (i32,i32,i32) host_call ABI.
    (local.set $scratch32 (i32.wrap_i64 (local.get $in_ptr)))
    (i32.store16 (i64.add (local.get $in_ptr) (i64.const 2)) (i32.const 96))
    (local.set $r
      (call $host_call (i32.const 0x04) (local.get $scratch32) (i32.const 0)))

    ;; 4. base = u64 at record offset 72; w0 = window[0] via the 64-bit base
    (local.set $base (i64.load (i64.add (local.get $in_ptr) (i64.const 72))))

    ;; 5. output (mem64: store address is i64; i32.store8 stores the low byte of
    ;;    an i32 value, i64.store writes the 8-byte base)
    (i32.store8 (local.get $out_ptr) (local.get $count))                 ;; [0] count
    (i32.store8 (i64.add (local.get $out_ptr) (i64.const 1))
                (i32.load8_u (local.get $base)))                         ;; [1] window[0]
    (i32.store8 (i64.add (local.get $out_ptr) (i64.const 2))
                (i32.eq (local.get $reject) (i32.const -1)))             ;; [2] reject ok
    (i64.store (i64.add (local.get $out_ptr) (i64.const 3))
               (local.get $base))                                       ;; [3..10] base LE
    (i32.store8 (i64.add (local.get $out_ptr) (i64.const 11))
                (i32.eq (local.get $r) (i32.const 96)))                  ;; [11] r ok

    (i32.const 12)
  )

  (func $hull_version (export "hull_version") (result i32)
    (i32.const 1)
  )
)
