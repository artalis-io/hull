// Generics: type parameters on declarations, type arguments after expressions.
function identity   (x   )    { return x; }
function pair                          (a   , b   )         { return [a, b]; }
async function load                                    (xs   )             { return xs; }
const f = function    (x   ) { return x; };
const arrow =     (x   )    => x;
const arrow2 =                   (x   ) => x.length;
const asyncArrow = async    (x   )             => x;
export default function    (x   ) { return x; }
class Box    extends Array    {
    value    ;
    map   (fn             )         { return new Box   (); }
    static of   (v   )         { const b = new Box   (); b.value = v; return b; }
    opt    (w   )       {}
}
class Plain extends Base                 {}
const Anon = class    extends Base    {};
const obj = { m   (x   )    { return x; }, async n   (x   ) { return x; } };
const m = new Map                       ();
const s = new Set        ;
const r = identity        (1);
const nested = pair                      (1, {});
const shifted = identity                      ([]);
const chained = a.b.c        (1).d        ();
const opt = maybe?.        ("x");
const tagged = tag        `a${1}b`;
const inst = identity        ;
const list = [identity        , pair                ];
call(identity         , 1);
const cond = flag ? identity         : identity        ;
const logical = identity         ?? fallback;
const split = identity        
(2);
// Comparisons stay comparisons.
const lt = a < b, gt = a > b;
const both = a < b > c;
for (let i = 0; i < n; i++) {}
const ge = x < y >= z;
const shift = x << y >> z;
const mixed = a < b && c > d;
const re = a < /x/.source.length;
const unary = a < b > -c;
   (x   ) => x;
