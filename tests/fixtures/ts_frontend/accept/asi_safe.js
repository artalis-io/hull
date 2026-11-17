// An erased declaration after a statement that ends in ';' (or a block) is safe.
let a = b;
               
(c)
function f() {}
              
[1].forEach(f)
const t = 1
               
let next = 2
const w = 1;
              ;

// A line after an `as` type that cannot continue the expression stays its own statement.
const e = v          
+1
const f2 = w     
next()
