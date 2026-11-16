// Typed arrow functions: plain, async, optional and rest parameters, defaults, return types.
const id = (x        )         => x;
const noArgs = ()       => {};
const opt = (a , b         ) => a;
const defs = (a         = 1, { b }                = { b: "" }, ...c          ) => a;
const asy = async (u        )                  => u;
const asyOpt = async (a         ) => a;
const nested = (a        ) => (b        )         => a + b;
const inCall = [1, 2].map((v        , i        )         => String(v + i));
const pick = flag ? (x        ) => x : (x        ) => -x;
const paren = (x) => x;
const conditional = cond ? (a) : b;
switch (k) { case (one): break; }
const ret = (a)         => a;
