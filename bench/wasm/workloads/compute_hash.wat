;; compute_hash.wat — Compute-intensive benchmark: iterative hash compression
;;
;; A simplified Merkle-Damgard-style hash over 8×u32 state.
;; For each 64-byte block: 64 rounds of mixing (shifts, XORs, rotations, adds).
;; Writes 32-byte digest to output.
;;
;; hull_process(in_ptr, in_len, out_ptr, out_max) -> 32 (bytes written)

(module
  (import "env" "host_call" (func $host_call (param i32 i32 i32) (result i32)))
  (memory (export "memory") 1)

  ;; Rotate right: (x >>> n) | (x << (32-n))
  (func $rotr (param $x i32) (param $n i32) (result i32)
    (i32.or
      (i32.shr_u (local.get $x) (local.get $n))
      (i32.shl (local.get $x) (i32.sub (i32.const 32) (local.get $n)))
    )
  )

  (func $hull_process (export "hull_process")
    (param $in_ptr i32) (param $in_len i32)
    (param $out_ptr i32) (param $out_max i32)
    (result i32)

    (local $s0 i32) (local $s1 i32) (local $s2 i32) (local $s3 i32)
    (local $s4 i32) (local $s5 i32) (local $s6 i32) (local $s7 i32)
    (local $off i32) (local $round i32)
    (local $a i32) (local $b i32) (local $t i32) (local $w i32)
    (local $block_end i32)

    ;; Need 32 bytes of output
    (if (i32.lt_u (local.get $out_max) (i32.const 32))
      (then (return (i32.const -2)))
    )

    ;; Initialize state (fixed IV — SHA-256-like initial values)
    (local.set $s0 (i32.const 0x6a09e667))
    (local.set $s1 (i32.const 0xbb67ae85))
    (local.set $s2 (i32.const 0x3c6ef372))
    (local.set $s3 (i32.const 0xa54ff53a))
    (local.set $s4 (i32.const 0x510e527f))
    (local.set $s5 (i32.const 0x9b05688c))
    (local.set $s6 (i32.const 0x1f83d9ab))
    (local.set $s7 (i32.const 0x5be0cd19))

    ;; Process each 64-byte block
    (local.set $off (i32.const 0))
    (block $done
      (loop $blocks
        ;; Compute how many bytes remain
        (br_if $done (i32.ge_u (local.get $off) (local.get $in_len)))

        ;; 64 rounds of mixing per block
        (local.set $round (i32.const 0))
        (block $round_done
          (loop $rounds
            (br_if $round_done (i32.ge_u (local.get $round) (i32.const 64)))

            ;; Load a message byte (wrap around input if off+round >= in_len)
            (local.set $w
              (i32.load8_u
                (i32.add
                  (local.get $in_ptr)
                  (i32.rem_u
                    (i32.add (local.get $off) (local.get $round))
                    (local.get $in_len)
                  )
                )
              )
            )

            ;; Sigma0 = rotr(s0, 2) ^ rotr(s0, 13) ^ rotr(s0, 22)
            (local.set $a
              (i32.xor
                (i32.xor
                  (call $rotr (local.get $s0) (i32.const 2))
                  (call $rotr (local.get $s0) (i32.const 13))
                )
                (call $rotr (local.get $s0) (i32.const 22))
              )
            )

            ;; Sigma1 = rotr(s4, 6) ^ rotr(s4, 11) ^ rotr(s4, 25)
            (local.set $b
              (i32.xor
                (i32.xor
                  (call $rotr (local.get $s4) (i32.const 6))
                  (call $rotr (local.get $s4) (i32.const 11))
                )
                (call $rotr (local.get $s4) (i32.const 25))
              )
            )

            ;; Ch = (s4 & s5) ^ (~s4 & s6)
            (local.set $t
              (i32.xor
                (i32.and (local.get $s4) (local.get $s5))
                (i32.and
                  (i32.xor (local.get $s4) (i32.const -1))
                  (local.get $s6)
                )
              )
            )

            ;; t1 = s7 + Sigma1 + Ch + w + round_constant(round)
            (local.set $t
              (i32.add
                (i32.add
                  (i32.add (local.get $s7) (local.get $b))
                  (local.get $t)
                )
                (i32.add
                  (local.get $w)
                  (i32.mul (local.get $round) (i32.const 0x5a827999))
                )
              )
            )

            ;; Maj = (s0 & s1) ^ (s0 & s2) ^ (s1 & s2)
            (local.set $a
              (i32.add
                (local.get $a)
                (i32.xor
                  (i32.xor
                    (i32.and (local.get $s0) (local.get $s1))
                    (i32.and (local.get $s0) (local.get $s2))
                  )
                  (i32.and (local.get $s1) (local.get $s2))
                )
              )
            )

            ;; Rotate state: s7=s6, s6=s5, s5=s4, s4=s3+t1, s3=s2, s2=s1, s1=s0, s0=t1+a
            (local.set $s7 (local.get $s6))
            (local.set $s6 (local.get $s5))
            (local.set $s5 (local.get $s4))
            (local.set $s4 (i32.add (local.get $s3) (local.get $t)))
            (local.set $s3 (local.get $s2))
            (local.set $s2 (local.get $s1))
            (local.set $s1 (local.get $s0))
            (local.set $s0 (i32.add (local.get $t) (local.get $a)))

            (local.set $round (i32.add (local.get $round) (i32.const 1)))
            (br $rounds)
          )
        )

        (local.set $off (i32.add (local.get $off) (i32.const 64)))
        (br $blocks)
      )
    )

    ;; Write state to output as 8 × i32 (little-endian)
    (i32.store (local.get $out_ptr) (local.get $s0))
    (i32.store (i32.add (local.get $out_ptr) (i32.const 4)) (local.get $s1))
    (i32.store (i32.add (local.get $out_ptr) (i32.const 8)) (local.get $s2))
    (i32.store (i32.add (local.get $out_ptr) (i32.const 12)) (local.get $s3))
    (i32.store (i32.add (local.get $out_ptr) (i32.const 16)) (local.get $s4))
    (i32.store (i32.add (local.get $out_ptr) (i32.const 20)) (local.get $s5))
    (i32.store (i32.add (local.get $out_ptr) (i32.const 24)) (local.get $s6))
    (i32.store (i32.add (local.get $out_ptr) (i32.const 28)) (local.get $s7))

    (i32.const 32)
  )

  (func $hull_version (export "hull_version") (result i32)
    (i32.const 1)
  )
)
