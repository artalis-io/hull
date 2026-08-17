--
-- hull.source.range — language-neutral SourceRange + line map.
--
-- A range is a half-open byte interval { start = <int>, stop = <int> } over the
-- 1-based bytes of the source (Lua string convention). `stop` is one-past-end,
-- so the exact text is source:sub(range.start, range.stop - 1). An empty range
-- has start == stop.
--
-- Line/column are resolved ON DEMAND from a per-source line-start index (built
-- once, binary-searched) rather than duplicated onto every AST node. Columns are
-- 1-based BYTE columns within the line (a UTF-8 codepoint column mode can be added
-- later without changing byte ranges). This module is Lua-agnostic and is reused
-- verbatim for the future JS source layer.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

-- Construct a half-open range. No validation here (callers pass lexer positions).
function M.new(start, stop)
    return { start = start, stop = stop }
end

-- Exact original text for a range over `source`.
function M.slice(source, r)
    return source:sub(r.start, r.stop - 1)
end

-- Build the line-start index: byte offset (1-based) of the first byte of each
-- line. Line 1 starts at offset 1; every byte immediately after a '\n' starts a
-- new line. CRLF ends its line at the '\n' (the '\r' is the line's trailing byte);
-- a final line without a trailing newline has no extra entry. Lone '\r' (classic
-- Mac) is NOT treated as a line terminator.
function M.linemap(source)
    local starts = { 1 }
    local i = 1
    local n = #source
    while i <= n do
        local nl = source:find("\n", i, true)
        if not nl then break end
        starts[#starts + 1] = nl + 1
        i = nl + 1
    end
    return starts
end

-- Resolve a 1-based byte offset to (line, col), both 1-based; col is the byte
-- column within the line. An offset one past end-of-source resolves to the last
-- line at the column after its last byte. Binary search for the largest
-- line-start <= off.
function M.position(starts, off)
    if off < 1 then off = 1 end
    local lo, hi = 1, #starts
    while lo < hi do
        local mid = (lo + hi + 1) // 2
        if starts[mid] <= off then lo = mid else hi = mid - 1 end
    end
    return lo, off - starts[lo] + 1
end

return M
