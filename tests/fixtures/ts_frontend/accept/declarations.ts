// Interfaces, type aliases and `declare` declarations erase to nothing.
interface User {
    name: string;
    age?: number;
    greet(other: User): string;
}
interface Admin<T> extends User, Record<string, T> { level: number }
type Id = string | number;
type Box<T> = { value: T };
type Handler = (req: Request) => Promise<Response>
declare const VERSION: string;
declare let counter: number, label;
declare var legacy;
declare function lookup(id: Id): User | undefined;
declare function identity<T>(x: T): T
const user = { name: "a" };
function local() {
    type Inner = number;
    interface Shape { area(): number }
    return 1;
}
switch (user.name) {
    case "a":
        type Case = string;
        break;
}
