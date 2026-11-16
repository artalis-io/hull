// Object types, index signatures, method / call / construct signatures, mapped types.
let o1: { a: string; b?: number, c: boolean };
let o2: { readonly a: string };
let o3: { [key: string]: number };
let o4: { (x: number): string; new (x: number): object; method(): void; opt?(a: string): number };
let o5: { get v(): number; set v(x: number) };
let o6: { [K in keyof T]?: T[K] };
let o7: { readonly [K in Keys as `get${K}`]-?: () => T[K] };
let o8: { +readonly [K in Keys]+?: string };
let o9: {
    multi: string
    line: number
};
let o10: { <T>(x: T): T; new <U>(): U };
let o11: {};
