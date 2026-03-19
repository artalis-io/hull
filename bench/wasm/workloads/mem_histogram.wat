;; mem_histogram.wat — Memory-intensive benchmark: histogram + counting sort
;;
;; 1. Build a 256-element frequency histogram of input bytes (random-access writes)
;; 2. Prefix-sum scan over histogram
;; 3. Counting sort: scatter output using cumulative counts
;; Writes sorted bytes to output.
;;
;; Uses a scratch area at offset 0 in linear memory (1024 bytes for 256×i32 histogram).
;; Input and output pointers are beyond that region (managed by host malloc).
;;
;; hull_process(in_ptr, in_len, out_ptr, out_max) -> in_len (bytes written)

(module
  (import "env" "host_call" (func $host_call (param i32 i32 i32) (result i32)))
  (memory (export "memory") 4)  ;; 4 pages = 256KB

  ;; Scratch area: bytes 0..1023 = histogram[256] (4 bytes each)
  ;; Host allocations start well beyond this.

  (func $hull_process (export "hull_process")
    (param $in_ptr i32) (param $in_len i32)
    (param $out_ptr i32) (param $out_max i32)
    (result i32)

    (local $i i32) (local $byte i32) (local $count i32)
    (local $hist_ptr i32) (local $sum i32)

    ;; Check output fits
    (if (i32.gt_u (local.get $in_len) (local.get $out_max))
      (then (return (i32.const -2)))
    )

    ;; Zero histogram (256 × 4 bytes = 1024 bytes at offset 0)
    ;; Use memory.fill for efficiency
    (memory.fill (i32.const 0) (i32.const 0) (i32.const 1024))

    ;; Pass 1: Build histogram — scan input, increment counts
    (local.set $i (i32.const 0))
    (block $hist_done
      (loop $hist_loop
        (br_if $hist_done (i32.ge_u (local.get $i) (local.get $in_len)))

        ;; byte = input[i]
        (local.set $byte
          (i32.load8_u (i32.add (local.get $in_ptr) (local.get $i)))
        )

        ;; histogram[byte]++
        (local.set $hist_ptr (i32.shl (local.get $byte) (i32.const 2)))
        (i32.store
          (local.get $hist_ptr)
          (i32.add (i32.load (local.get $hist_ptr)) (i32.const 1))
        )

        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $hist_loop)
      )
    )

    ;; Pass 2: Prefix sum (in-place, inclusive scan)
    (local.set $sum (i32.const 0))
    (local.set $i (i32.const 0))
    (block $prefix_done
      (loop $prefix_loop
        (br_if $prefix_done (i32.ge_u (local.get $i) (i32.const 256)))

        (local.set $hist_ptr (i32.shl (local.get $i) (i32.const 2)))
        (local.set $count (i32.load (local.get $hist_ptr)))
        (local.set $sum (i32.add (local.get $sum) (local.get $count)))
        (i32.store (local.get $hist_ptr) (local.get $sum))

        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $prefix_loop)
      )
    )

    ;; Pass 3: Counting sort — iterate input backwards, place into output
    (local.set $i (local.get $in_len))
    (block $sort_done
      (loop $sort_loop
        (br_if $sort_done (i32.eqz (local.get $i)))
        (local.set $i (i32.sub (local.get $i) (i32.const 1)))

        ;; byte = input[i]
        (local.set $byte
          (i32.load8_u (i32.add (local.get $in_ptr) (local.get $i)))
        )

        ;; Decrement prefix sum: histogram[byte]--
        (local.set $hist_ptr (i32.shl (local.get $byte) (i32.const 2)))
        (local.set $count
          (i32.sub (i32.load (local.get $hist_ptr)) (i32.const 1))
        )
        (i32.store (local.get $hist_ptr) (local.get $count))

        ;; output[count] = byte
        (i32.store8
          (i32.add (local.get $out_ptr) (local.get $count))
          (local.get $byte)
        )

        (br $sort_loop)
      )
    )

    ;; Return bytes written = in_len
    (local.get $in_len)
  )

  (func $hull_version (export "hull_version") (result i32)
    (i32.const 1)
  )
)
