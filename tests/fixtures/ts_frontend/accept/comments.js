// Comments inside type syntax are erased with it; JSDoc before a declaration still attaches.
let a                      = 1;
let b         // trailing
    = "b";
/** @route GET /users */
export function users(req          /* the request */, res          )       {}
/**
 * @param n a number
 */
const twice = (n        )         => n * 2;
