// Ambient declarations erase whole, bodies included.
declare class Logger<T> extends Base<T> implements Sink {
    constructor(name: string);
    log(msg: string): void;
    static level: number;
    private readonly secret;
    [key: string]: unknown;
}
declare abstract class Shape { abstract area(): number; }
declare enum Color { Red, Green = 2, Blue = Green << 1 }
declare const enum Flags { A = 1, B = A | 2, C = 8 / 2 }
declare namespace NS {
    export const x: number;
    export function f(): void;
    namespace Inner { type T = `a${string}`; }
}
declare namespace A.B.C { }
declare module "pkg" {
    export default function main(): void;
    import x = require("dep");
    export = x;
}
declare module "shorthand";
declare global {
    interface Window { hull: unknown; }
    var __DEV__: boolean;
}
declare interface Point { x: number }
declare type Pair = [number, number];
export declare class Exported {}
export declare namespace ExportedNS {}
export declare enum ExportedEnum { A }
export declare const exportedConst: number;
import type fs = require("fs");
import type Alias = NS.Inner;
export const ok = 1;
