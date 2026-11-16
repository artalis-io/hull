// Parameter and return types, optional and rest parameters, type predicates.
function add(a: number, b: number): number { return a + b; }
function opt(a?: string, b: number = 2, ...rest: boolean[]): void {}
function pattern({ x, y }: { x: number; y: number }, [z]: number[] = []): number { return x + y + z; }
async function load(url: string): Promise<Response> { return fetch(url); }
function isStr(v: unknown): v is string { return typeof v === "string"; }
function assertStr(v: unknown): asserts v is string {}
function assertOk(v: unknown): asserts v {}
const fe = function (n: number): number { return n * 2; };
const obj = {
    method(a: string): string { return a; },
    get prop(): number { return 1; },
    set prop(v: number) {},
    async run(x?: number): Promise<void> {},
};
export default function main(args: string[]): number { return args.length; }
