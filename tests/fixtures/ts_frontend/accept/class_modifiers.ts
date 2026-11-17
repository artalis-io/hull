// Class and member modifiers, abstract and declare members, index signatures, implements.
abstract class Shape extends Root implements Drawable, Named<string> {
    abstract area(): number;
    abstract readonly sides: number;
    declare readonly tag: string;
    [key: string]: unknown;
    static [index: number]: string;
    public name: string = "shape";
    private secret = 1;
    protected readonly id = 2;
    public static readonly count: number = 0;
    static override readonly version = 3;
    override toString(): string { return this.name; }
    protected abstract draw(ctx: Context): void
    public get size(): number { return 1; }
    private set size(v: number) {}
    public async load(): Promise<void> {}
    private static helper() {}
    public constructor(name: string) { this.name = name; }
}
export abstract class Base {}
class Impl extends Shape implements Drawable {
    area(): number { return 0; }
    sides = 4;
    draw(): void {}
}
const Anon = class implements Drawable { draw() {} };
class Names {
    public = 1;
    private;
    readonly
    x = 2;
    static
    y = 3;
    declare: number;
    abstract?: string;
    static readonly: boolean;
}
class Spaced {
    a = 1;
    declare b: string;
    private c = 2
    d = 3
    private e = 4;
}
