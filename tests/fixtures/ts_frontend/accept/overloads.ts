// Overload signatures erase with their statement or member; `this` parameters erase.
function pick(a: string): string;
function pick(a: number): number;
function pick(a: any) { return a; }
export function both(a: string): void;
export function both(a: number): void;
export function both(a: unknown) {}
async function later(a: string): Promise<string>;
async function later(a: any) { return a; }
export async function load(a: string): Promise<string>;
export async function load(a: any) { return a; }
export default function main(argv: string[]): number;
export default function main(argv: string[]) { return argv.length; }
function untyped(a)
function untyped(a) { return a; }
class Parser {
    constructor(src: string);
    constructor(src: Uint8Array);
    constructor(src: any) {}
    parse(x: string): number;
    parse(x: number): number;
    parse(x: any) { return 1; }
    static of(): Parser
    static of(x?: string) { return new Parser(""); }
    maybe?(): void;
}
function inner() {
    function local(x: string): void;
    function local(x: any) {}
    return local;
}
function onClick(this: HTMLElement, ev: Event) {}
function only(this: Window) {}
const handler = function (this: Node, a: number,) { return a; };
const o = { m(this: typeof o) { return this; } };
class K { m(this: K, x: number) { return x; } }
