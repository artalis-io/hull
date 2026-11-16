// `as`, `as const`, `satisfies` and non-null assertions.
const a = value as string;
const b = value as unknown as number[];
const c = [1, 2, 3] as const;
const d = { k: 1 } satisfies Record<string, number>;
const e = maybe!.field!.other;
const f = list![0]!;
const g = fn!();
obj!.x = 1;
counter!++;
const h = (raw as any).deep;
const i = a < b as boolean;
const j = x + y as number;
const k = n as number / 2;
const l = cond ? v as string : w;
