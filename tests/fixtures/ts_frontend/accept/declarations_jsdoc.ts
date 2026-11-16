// JSDoc attaches as it would to the erased JavaScript.
/** @route GET /users */
interface UsersQuery { page: number }
/** @route POST /users */
export function create(q: UsersQuery): void {}
interface Inline {} /** @tag same line */
function after() {}
