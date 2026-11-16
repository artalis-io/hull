// Parameter and return types, optional and rest parameters, type predicates.
function add(a        , b        )         { return a + b; }
function opt(a         , b         = 2, ...rest           )       {}
function pattern({ x, y }                          , [z]           = [])         { return x + y + z; }
async function load(url        )                    { return fetch(url); }
function isStr(v         )              { return typeof v === "string"; }
function assertStr(v         )                      {}
function assertOk(v         )            {}
const fe = function (n        )         { return n * 2; };
const obj = {
    method(a        )         { return a; },
    get prop()         { return 1; },
    set prop(v        ) {},
    async run(x         )                {},
};
export default function main(args          )         { return args.length; }
