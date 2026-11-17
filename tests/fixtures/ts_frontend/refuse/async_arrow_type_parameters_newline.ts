// A line break between an async arrow's type parameters and its parameters: TypeScript reads
// an async arrow, but erased, `async` before a line break is not one.
const f = async <T>
(x: T) => x;
