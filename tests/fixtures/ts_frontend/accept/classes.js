// Class fields and members with types.
class Point {
    x         = 0;
    y         ;
    label         ;
    static origin       ;
    constructor(x        , y        ) { this.x = x; this.y = y; }
    move(dx        , dy         = 0)        { return this; }
    get length()         { return 0; }
    set length(v        ) {}
    static make()        { return new Point(0, 0); }
    async fetch (url        )                {}
}
export class Named extends Point {
    name         = "n";
}
