;; kv_store.wat — Stateful WASM module for persistent instance testing
;;
;; Demonstrates the value of persistent instances: load state once,
;; query many times with preserved linear memory.
;;
;; Protocol:
;;   Input byte 0 = opcode:
;;     0x01 = LOAD: remaining bytes are key-value pairs
;;            Format: [count:u32] [key_len:u32 key_bytes... val_len:u32 val_bytes...]...
;;            Returns: 4 bytes (u32 count of entries loaded)
;;
;;     0x02 = GET: remaining bytes are the key to look up
;;            Returns: value bytes if found, empty (0 bytes) if not found
;;
;;     0x03 = COUNT: no payload
;;            Returns: 4 bytes (u32 count of loaded entries)
;;
;; Linear memory layout:
;;   Page 0-3 (0x0000-0xFFFF): reserved for hull I/O buffers
;;   Page 4+  (0x10000+):      persistent key-value store
;;
;; Global state preserved across calls on persistent instances:
;;   $store_base  = start of KV data in memory
;;   $store_count = number of entries
;;   $store_end   = byte past last entry (next write position)

(module
  (import "env" "host_call" (func $host_call (param i32 i32 i32) (result i32)))

  ;; 16 pages = 1 MB — enough for a reasonable KV store
  (memory (export "memory") 16)

  ;; Persistent globals — survive across calls
  (global $store_base  (mut i32) (i32.const 0x10000))  ;; start at page 4
  (global $store_count (mut i32) (i32.const 0))
  (global $store_end   (mut i32) (i32.const 0x10000))

  (func $hull_process (export "hull_process")
    (param $in_ptr i32) (param $in_len i32)
    (param $out_ptr i32) (param $out_max i32)
    (result i32)

    (local $opcode i32)
    (local $off i32)
    (local $count i32)
    (local $key_len i32)
    (local $val_len i32)
    (local $dst i32)
    (local $i i32)
    (local $scan i32)
    (local $s_key_len i32)
    (local $s_val_len i32)
    (local $match i32)
    (local $j i32)
    (local $query_key_ptr i32)
    (local $query_key_len i32)

    ;; Need at least 1 byte for opcode
    (if (i32.lt_u (local.get $in_len) (i32.const 1))
      (then (return (i32.const -1)))
    )

    ;; Read opcode
    (local.set $opcode (i32.load8_u (local.get $in_ptr)))

    ;; ── LOAD (0x01) ──────────────────────────────────────────────
    (if (i32.eq (local.get $opcode) (i32.const 1))
      (then
        ;; Need at least 5 bytes: opcode + count(u32)
        (if (i32.lt_u (local.get $in_len) (i32.const 5))
          (then (return (i32.const -1)))
        )

        ;; Read entry count
        (local.set $count (i32.load (i32.add (local.get $in_ptr) (i32.const 1))))

        ;; Reset store
        (global.set $store_count (i32.const 0))
        (global.set $store_end (global.get $store_base))
        (local.set $dst (global.get $store_base))
        (local.set $off (i32.const 5))  ;; past opcode + count

        ;; Copy entries into persistent memory
        (local.set $i (i32.const 0))
        (block $load_done
          (loop $load_loop
            (br_if $load_done (i32.ge_u (local.get $i) (local.get $count)))

            ;; Read key_len (u32)
            (if (i32.gt_u (i32.add (local.get $off) (i32.const 4)) (local.get $in_len))
              (then (br $load_done))
            )
            (local.set $key_len (i32.load (i32.add (local.get $in_ptr) (local.get $off))))
            (local.set $off (i32.add (local.get $off) (i32.const 4)))

            ;; Write key_len to store
            (i32.store (local.get $dst) (local.get $key_len))
            (local.set $dst (i32.add (local.get $dst) (i32.const 4)))

            ;; Copy key bytes
            (if (i32.gt_u (local.get $key_len) (i32.const 0))
              (then
                (memory.copy
                  (local.get $dst)
                  (i32.add (local.get $in_ptr) (local.get $off))
                  (local.get $key_len)
                )
              )
            )
            (local.set $dst (i32.add (local.get $dst) (local.get $key_len)))
            (local.set $off (i32.add (local.get $off) (local.get $key_len)))

            ;; Read val_len (u32)
            (if (i32.gt_u (i32.add (local.get $off) (i32.const 4)) (local.get $in_len))
              (then (br $load_done))
            )
            (local.set $val_len (i32.load (i32.add (local.get $in_ptr) (local.get $off))))
            (local.set $off (i32.add (local.get $off) (i32.const 4)))

            ;; Write val_len to store
            (i32.store (local.get $dst) (local.get $val_len))
            (local.set $dst (i32.add (local.get $dst) (i32.const 4)))

            ;; Copy val bytes
            (if (i32.gt_u (local.get $val_len) (i32.const 0))
              (then
                (memory.copy
                  (local.get $dst)
                  (i32.add (local.get $in_ptr) (local.get $off))
                  (local.get $val_len)
                )
              )
            )
            (local.set $dst (i32.add (local.get $dst) (local.get $val_len)))
            (local.set $off (i32.add (local.get $off) (local.get $val_len)))

            (global.set $store_count (i32.add (global.get $store_count) (i32.const 1)))

            (local.set $i (i32.add (local.get $i) (i32.const 1)))
            (br $load_loop)
          )
        )

        (global.set $store_end (local.get $dst))

        ;; Write count to output
        (if (i32.lt_u (local.get $out_max) (i32.const 4))
          (then (return (i32.const -2)))
        )
        (i32.store (local.get $out_ptr) (global.get $store_count))
        (return (i32.const 4))
      )
    )

    ;; ── GET (0x02) ────────────────────────────────────────────────
    (if (i32.eq (local.get $opcode) (i32.const 2))
      (then
        ;; Key is bytes 1..in_len
        (local.set $query_key_ptr (i32.add (local.get $in_ptr) (i32.const 1)))
        (local.set $query_key_len (i32.sub (local.get $in_len) (i32.const 1)))

        ;; Scan through stored entries
        (local.set $scan (global.get $store_base))
        (local.set $i (i32.const 0))
        (block $get_done
          (loop $get_loop
            (br_if $get_done (i32.ge_u (local.get $i) (global.get $store_count)))
            (br_if $get_done (i32.ge_u (local.get $scan) (global.get $store_end)))

            ;; Read stored key_len
            (local.set $s_key_len (i32.load (local.get $scan)))
            (local.set $scan (i32.add (local.get $scan) (i32.const 4)))

            ;; Compare key lengths
            (if (i32.eq (local.get $s_key_len) (local.get $query_key_len))
              (then
                ;; Compare key bytes
                (local.set $match (i32.const 1))
                (local.set $j (i32.const 0))
                (block $cmp_done
                  (loop $cmp_loop
                    (br_if $cmp_done (i32.ge_u (local.get $j) (local.get $s_key_len)))
                    (if (i32.ne
                          (i32.load8_u (i32.add (local.get $scan) (local.get $j)))
                          (i32.load8_u (i32.add (local.get $query_key_ptr) (local.get $j))))
                      (then
                        (local.set $match (i32.const 0))
                        (br $cmp_done)
                      )
                    )
                    (local.set $j (i32.add (local.get $j) (i32.const 1)))
                    (br $cmp_loop)
                  )
                )

                (if (local.get $match)
                  (then
                    ;; Skip past key to val_len
                    (local.set $scan (i32.add (local.get $scan) (local.get $s_key_len)))
                    (local.set $s_val_len (i32.load (local.get $scan)))
                    (local.set $scan (i32.add (local.get $scan) (i32.const 4)))

                    ;; Check output capacity
                    (if (i32.gt_u (local.get $s_val_len) (local.get $out_max))
                      (then (return (i32.const -2)))
                    )

                    ;; Copy value to output
                    (if (i32.gt_u (local.get $s_val_len) (i32.const 0))
                      (then
                        (memory.copy
                          (local.get $out_ptr)
                          (local.get $scan)
                          (local.get $s_val_len)
                        )
                      )
                    )
                    (return (local.get $s_val_len))
                  )
                )
              )
            )

            ;; Skip past this entry's key + val_len + val
            (local.set $scan (i32.add (local.get $scan) (local.get $s_key_len)))
            (local.set $s_val_len (i32.load (local.get $scan)))
            (local.set $scan (i32.add (local.get $scan) (i32.const 4)))
            (local.set $scan (i32.add (local.get $scan) (local.get $s_val_len)))

            (local.set $i (i32.add (local.get $i) (i32.const 1)))
            (br $get_loop)
          )
        )

        ;; Not found — return 0 bytes
        (return (i32.const 0))
      )
    )

    ;; ── COUNT (0x03) ──────────────────────────────────────────────
    (if (i32.eq (local.get $opcode) (i32.const 3))
      (then
        (if (i32.lt_u (local.get $out_max) (i32.const 4))
          (then (return (i32.const -2)))
        )
        (i32.store (local.get $out_ptr) (global.get $store_count))
        (return (i32.const 4))
      )
    )

    ;; Unknown opcode
    (i32.const -1)
  )

  (func $hull_version (export "hull_version") (result i32)
    (i32.const 1)
  )
)
