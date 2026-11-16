// Generics: type parameters on declarations, type arguments after expressions.
function identity<T>(x: T): T { return x; }
function pair<A, B extends object = {}>(a: A, b: B): [A, B] { return [a, b]; }
async function load<const T extends readonly unknown[]>(xs: T): Promise<T> { return xs; }
const f = function <T>(x: T) { return x; };
const arrow = <T,>(x: T): T => x;
const arrow2 = <T extends string>(x: T) => x.length;
const asyncArrow = async <T>(x: T): Promise<T> => x;
export default function <T>(x: T) { return x; }
class Box<T> extends Array<T> {
    value?: T;
    map<U>(fn: (v: T) => U): Box<U> { return new Box<U>(); }
    static of<V>(v: V): Box<V> { const b = new Box<V>(); b.value = v; return b; }
    opt?<W>(w: W): void {}
}
class Plain extends Base<string, number> {}
const Anon = class<T> extends Base<T> {};
const obj = { m<T>(x: T): T { return x; }, async n<T>(x: T) { return x; } };
const m = new Map<string, Array<number>>();
const s = new Set<string>;
const r = identity<number>(1);
const nested = pair<Box<Box<number>>, {}>(1, {});
const shifted = identity<Array<Array<number>>>([]);
const chained = a.b.c<string>(1).d<number>();
const opt = maybe?.<string>("x");
const tagged = tag<string>`a${1}b`;
const inst = identity<string>;
const list = [identity<number>, pair<string, number>];
call(identity<boolean>, 1);
const cond = flag ? identity<number> : identity<string>;
const logical = identity<number> ?? fallback;
const split = identity<number>
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
<T>(x: T) => x;
