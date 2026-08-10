(module
  (memory (export "memory") 1)
  (func (export "store_i32") (param $p i32)
    local.get $p (i32.const 0x11223344) i32.store)
  (func (export "load_i32") (param $p i32) (result i32)
    local.get $p i32.load))
