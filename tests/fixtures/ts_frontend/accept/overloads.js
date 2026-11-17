// Overload signatures erase with their statement or member; `this` parameters erase.
                                 
                                 
function pick(a     ) { return a; }
                                      
                                      
export function both(a         ) {}
                                                 
async function later(a     ) { return a; }
                                                       
export async function load(a     ) { return a; }
                                                     
export default function main(argv          ) { return argv.length; }
                   
function untyped(a) { return a; }
class Parser {
                             
                                 
    constructor(src     ) {}
                             
                             
    parse(x     ) { return 1; }
                       
    static of(x         ) { return new Parser(""); }
                   
}
function inner() {
                                    
    function local(x     ) {}
    return local;
}
function onClick(                   ev       ) {}
function only(            ) {}
const handler = function (            a        ,) { return a; };
const o = { m(              ) { return this; } };
class K { m(         x        ) { return x; } }
