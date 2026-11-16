// Class fields and members with types.
class Point {
    x: number = 0;
    y?: number;
    label!: string;
    static origin: Point;
    constructor(x: number, y: number) { this.x = x; this.y = y; }
    move(dx: number, dy: number = 0): Point { return this; }
    get length(): number { return 0; }
    set length(v: number) {}
    static make(): Point { return new Point(0, 0); }
    async fetch?(url: string): Promise<void> {}
}
export class Named extends Point {
    name: string = "n";
}
