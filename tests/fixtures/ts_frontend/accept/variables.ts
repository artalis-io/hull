// Annotations on variable declarations.
let a: number = 1;
const b: string = "b", c: boolean = true;
var d: number[];
let e!: Map<string, number>;
const { f, g }: { f: number; g: string } = { f: 1, g: "g" };
const [h, i]: [number, string] = [1, "i"];
let j: number | undefined, k = 2, l: string;
for (const m of [1, 2]) { let n: number = m; }
export const o: readonly string[] = [];
export let p: Record<string, unknown> = {};
