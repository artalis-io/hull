;; shared_read.wat — WASM module that reads from shared heap data segments via host_call
;;
;; Exports:
;;   hull_process(in_ptr, in_len, out_ptr, out_max) -> bytes_written
;;   hull_version() -> 1
;;
;; Protocol:
;;   Input byte 0 = segment_id (0xFF = count query)
;;   Bytes 1-4 = offset (little-endian u32)
;;   Bytes 5-8 = length (little-endian u32)
;;
;;   Count query (segment_id == 0xFF):
;;     Calls host_call(0x02, -1, 0) to get segment count.
;;     Writes count as u32 to output, returns 4.
;;
;;   Segment read:
;;     host_call(0x02, segment_id, 0) -> WASM base address
;;     host_call(0x02, segment_id, 1) -> segment size
;;     If base == 0, return 0 (no data)
;;     If offset + length > size, return -1
;;     If length > out_max, return -2
;;     Copy length bytes from (base + offset) to out_ptr, return length

(module
  (import "env" "host_call" (func $host_call (param i32 i32 i32) (result i32)))

  (memory (export "memory") 1)

  (func $hull_process (export "hull_process")
    (param $in_ptr i32) (param $in_len i32)
    (param $out_ptr i32) (param $out_max i32)
    (result i32)

    (local $seg_id i32)
    (local $offset i32)
    (local $length i32)
    (local $count i32)
    (local $base i32)
    (local $size i32)
    (local $i i32)

    ;; Need at least 1 byte for segment_id
    (if (i32.lt_u (local.get $in_len) (i32.const 1))
      (then (return (i32.const -1)))
    )

    ;; Read segment_id
    (local.set $seg_id (i32.load8_u (local.get $in_ptr)))

    ;; ── COUNT QUERY (segment_id == 0xFF) ────────────────────────────
    (if (i32.eq (local.get $seg_id) (i32.const 0xFF))
      (then
        ;; Get segment count: host_call(0x02, -1, 0)
        (local.set $count (call $host_call (i32.const 0x02) (i32.const -1) (i32.const 0)))

        ;; Need at least 4 bytes of output space
        (if (i32.lt_u (local.get $out_max) (i32.const 4))
          (then (return (i32.const -2)))
        )

        ;; Write count as u32
        (i32.store (local.get $out_ptr) (local.get $count))
        (return (i32.const 4))
      )
    )

    ;; ── SEGMENT READ ────────────────────────────────────────────────
    ;; Need at least 9 bytes: 1 (seg_id) + 4 (offset) + 4 (length)
    (if (i32.lt_u (local.get $in_len) (i32.const 9))
      (then (return (i32.const -1)))
    )

    ;; Read offset (bytes 1-4, little-endian u32)
    (local.set $offset (i32.load (i32.add (local.get $in_ptr) (i32.const 1))))

    ;; Read length (bytes 5-8, little-endian u32)
    (local.set $length (i32.load (i32.add (local.get $in_ptr) (i32.const 5))))

    ;; Get base address: host_call(0x02, segment_id, 0)
    (local.set $base (call $host_call (i32.const 0x02) (local.get $seg_id) (i32.const 0)))

    ;; If base == 0, no data
    (if (i32.eqz (local.get $base))
      (then (return (i32.const 0)))
    )

    ;; Get segment size: host_call(0x02, segment_id, 1)
    (local.set $size (call $host_call (i32.const 0x02) (local.get $seg_id) (i32.const 1)))

    ;; If offset + length > size, return -1
    (if (i32.gt_u (i32.add (local.get $offset) (local.get $length)) (local.get $size))
      (then (return (i32.const -1)))
    )

    ;; If length > out_max, return -2
    (if (i32.gt_u (local.get $length) (local.get $out_max))
      (then (return (i32.const -2)))
    )

    ;; Copy length bytes from (base + offset) to out_ptr, byte-by-byte
    (local.set $i (i32.const 0))
    (block $copy_done
      (loop $copy_loop
        (br_if $copy_done (i32.ge_u (local.get $i) (local.get $length)))

        (i32.store8
          (i32.add (local.get $out_ptr) (local.get $i))
          (i32.load8_u (i32.add (i32.add (local.get $base) (local.get $offset)) (local.get $i)))
        )

        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $copy_loop)
      )
    )

    ;; Return length (bytes written)
    (local.get $length)
  )

  (func $hull_version (export "hull_version") (result i32)
    (i32.const 1)
  )
)
