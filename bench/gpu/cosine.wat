;; cosine.wat — Cosine similarity for WASM benchmark
;;
;; hull_process ABI: (in_ptr, in_len, out_ptr, out_max) -> bytes_written
;; Input:  [dims:u32] [count:u32] [query: dims×f32] [cands: count×dims×f32]
;; Output: [results: count×f32]

(module
  (memory (export "memory") 256)  ;; 16 MB

  (func (export "hull_process")
    (param $in_ptr i32) (param $in_len i32)
    (param $out_ptr i32) (param $out_max i32)
    (result i32)

    (local $dims i32)
    (local $count i32)
    (local $c i32)
    (local $d i32)
    (local $dot f32)
    (local $nq f32)
    (local $nc f32)
    (local $qv f32)
    (local $cv f32)
    (local $denom f32)
    (local $query_base i32)
    (local $cand_base i32)
    (local $cand_off i32)

    ;; Read dims and count
    (local.set $dims (i32.load (local.get $in_ptr)))
    (local.set $count (i32.load (i32.add (local.get $in_ptr) (i32.const 4))))

    ;; query_base = in_ptr + 8
    (local.set $query_base (i32.add (local.get $in_ptr) (i32.const 8)))
    ;; cand_base = query_base + dims * 4
    (local.set $cand_base
      (i32.add (local.get $query_base)
               (i32.mul (local.get $dims) (i32.const 4))))

    ;; For each candidate
    (local.set $c (i32.const 0))
    (block $break
      (loop $candidate_loop
        (br_if $break (i32.ge_u (local.get $c) (local.get $count)))

        ;; cand_off = cand_base + c * dims * 4
        (local.set $cand_off
          (i32.add (local.get $cand_base)
                   (i32.mul (i32.mul (local.get $c) (local.get $dims))
                            (i32.const 4))))

        (local.set $dot (f32.const 0))
        (local.set $nq (f32.const 0))
        (local.set $nc (f32.const 0))

        ;; Inner loop over dimensions
        (local.set $d (i32.const 0))
        (block $inner_break
          (loop $dim_loop
            (br_if $inner_break (i32.ge_u (local.get $d) (local.get $dims)))

            ;; qv = query[d]
            (local.set $qv
              (f32.load (i32.add (local.get $query_base)
                                 (i32.mul (local.get $d) (i32.const 4)))))
            ;; cv = candidates[c * dims + d]
            (local.set $cv
              (f32.load (i32.add (local.get $cand_off)
                                 (i32.mul (local.get $d) (i32.const 4)))))

            (local.set $dot (f32.add (local.get $dot)
                                     (f32.mul (local.get $qv) (local.get $cv))))
            (local.set $nq (f32.add (local.get $nq)
                                     (f32.mul (local.get $qv) (local.get $qv))))
            (local.set $nc (f32.add (local.get $nc)
                                     (f32.mul (local.get $cv) (local.get $cv))))

            (local.set $d (i32.add (local.get $d) (i32.const 1)))
            (br $dim_loop)
          )
        )

        ;; denom = sqrt(nq) * sqrt(nc)
        (local.set $denom
          (f32.mul (f32.sqrt (local.get $nq)) (f32.sqrt (local.get $nc))))

        ;; results[c] = (denom > 0) ? dot / denom : 0
        (f32.store
          (i32.add (local.get $out_ptr) (i32.mul (local.get $c) (i32.const 4)))
          (if (result f32) (f32.gt (local.get $denom) (f32.const 0))
            (then (f32.div (local.get $dot) (local.get $denom)))
            (else (f32.const 0))
          )
        )

        (local.set $c (i32.add (local.get $c) (i32.const 1)))
        (br $candidate_loop)
      )
    )

    ;; Return bytes written
    (i32.mul (local.get $count) (i32.const 4))
  )

  (func (export "hull_version") (result i32) (i32.const 1))
)
