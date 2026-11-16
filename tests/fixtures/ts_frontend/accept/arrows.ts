// Typed arrow functions: plain, async, optional and rest parameters, defaults, return types.
const id = (x: number): number => x;
const noArgs = (): void => {};
const opt = (a?, b?: string) => a;
const defs = (a: number = 1, { b }: { b: string } = { b: "" }, ...c: number[]) => a;
const asy = async (u: string): Promise<string> => u;
const asyOpt = async (a?: number) => a;
const nested = (a: number) => (b: number): number => a + b;
const inCall = [1, 2].map((v: number, i: number): string => String(v + i));
const pick = flag ? (x: number) => x : (x: number) => -x;
const paren = (x) => x;
const conditional = cond ? (a) : b;
switch (k) { case (one): break; }
const ret = (a): string => a;
