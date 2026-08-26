--- QR Code generator (ISO/IEC 18004).
--
-- @module hull.qrcode
-- @license AGPL-3.0-or-later
--
-- Pure Lua, no vendored deps. Implements byte mode only (handles any
-- ASCII / Latin-1 content), all four error correction levels (L 7%,
-- M 15%, Q 25%, H 30%), versions 1-40, all 8 mask patterns scored
-- per spec section 8.8.2.
--
-- ## Provenance
--
-- The algorithm structure (data-segment encoding, Reed-Solomon math,
-- module placement, mask scoring) is adapted from Project Nayuki's
-- "QR Code generator library" (MIT-licensed, https://www.nayuki.io/
-- page/qr-code-generator-library). Tables transcribed from
-- ISO/IEC 18004:2015 Annex E + Table 9, cross-checked against
-- Nayuki's reference. All wire-level math is in 0-indexed
-- coordinates (matching the spec); 1-indexed conversion happens
-- only at matrix access.
--
-- ## API
--
--   qrcode.encode(text [, opts]) -> {
--       matrix = { {0,1,0,...}, ... },   -- 2D module grid (1-indexed,
--                                        --   row, col), 1 = dark
--       size = N,                         -- N x N modules
--       version = V,                      -- 1..40
--       ec_level = "L"|"M"|"Q"|"H",
--       mask = 0..7,                      -- which mask was applied
--   }
--   qrcode.svg(text [, opts]) -> svg string
--
-- ## Common use cases
--
--   * TOTP enrollment QR (otpauth:// URLs, ~150-200 bytes typical)
--   * WiFi network share codes
--   * Contact cards (MeCard / vCard)
--   * Generic URLs / payment links / etc.

local M = {}

-- ── EC block layout (ISO/IEC 18004 Table 9) ────────────────────────
--
-- Per (ec_level, version) tuple, 5 numbers:
--   { ec_per_block, group1_blocks, g1_data_per_block,
--                   group2_blocks, g2_data_per_block }
--
-- Keyed [ec][version]. Internal ec ints: 1=L, 2=M, 3=Q, 4=H.
-- (Wire-format ec bits are different: L=01, M=00, Q=11, H=10.)

-- stylua: ignore start
local EC_TABLE = {
    -- L: 7%
    [1] = {
        { 7,  1,  19,  0,  0},  { 10,  1,  34,  0,  0},  { 15,  1,  55,  0,  0},
        {20,  1,  80,  0,  0},  { 26,  1, 108,  0,  0},  { 18,  2,  68,  0,  0},
        {20,  2,  78,  0,  0},  { 24,  2,  97,  0,  0},  { 30,  2, 116,  0,  0},
        {18,  2,  68,  2,  69}, { 20,  4,  81,  0,  0},  { 24,  2,  92,  2,  93},
        {26,  4, 107,  0,  0},  { 30,  3, 115,  1, 116}, { 22,  5,  87,  1,  88},
        {24,  5,  98,  1,  99}, { 28,  1, 107,  5, 108}, { 30,  5, 120,  1, 121},
        {28,  3, 113,  4, 114}, { 28,  3, 107,  5, 108}, { 28,  4, 116,  4, 117},
        {28,  2, 111,  7, 112}, { 30,  4, 121,  5, 122}, { 30,  6, 117,  4, 118},
        {26,  8, 106,  4, 107}, { 28, 10, 114,  2, 115}, { 30,  8, 122,  4, 123},
        {30,  3, 117, 10, 118}, { 30,  7, 116,  7, 117}, { 30,  5, 115, 10, 116},
        {30, 13, 115,  3, 116}, { 30, 17, 115,  0,  0},  { 30, 17, 115,  1, 116},
        {30, 13, 115,  6, 116}, { 30, 12, 121,  7, 122}, { 30,  6, 121, 14, 122},
        {30, 17, 122,  4, 123}, { 30,  4, 122, 18, 123}, { 30, 20, 117,  4, 118},
        {30, 19, 118,  6, 119},
    },
    -- M: 15%
    [2] = {
        {10,  1,  16,  0,  0},  { 16,  1,  28,  0,  0},  { 26,  1,  44,  0,  0},
        {18,  2,  32,  0,  0},  { 24,  2,  43,  0,  0},  { 16,  4,  27,  0,  0},
        {18,  4,  31,  0,  0},  { 22,  2,  38,  2,  39}, { 22,  3,  36,  2,  37},
        {26,  4,  43,  1,  44}, { 30,  1,  50,  4,  51}, { 22,  6,  36,  2,  37},
        {22,  8,  37,  1,  38}, { 24,  4,  40,  5,  41}, { 24,  5,  41,  5,  42},
        {28,  7,  45,  3,  46}, { 28, 10,  46,  1,  47}, { 26,  9,  43,  4,  44},
        {26,  3,  44, 11,  45}, { 26,  3,  41, 13,  42}, { 26, 17,  42,  0,  0},
        {28, 17,  46,  0,  0},  { 28,  4,  47, 14,  48}, { 28,  6,  45, 14,  46},
        {28,  8,  47, 13,  48}, { 28, 19,  46,  4,  47}, { 28, 22,  45,  3,  46},
        {28,  3,  45, 23,  46}, { 28, 21,  45,  7,  46}, { 28, 19,  47, 10,  48},
        {28,  2,  46, 29,  47}, { 28, 10,  46, 23,  47}, { 28, 14,  46, 21,  47},
        {28, 14,  46, 23,  47}, { 28, 12,  47, 26,  48}, { 28,  6,  47, 34,  48},
        {28, 29,  46, 14,  47}, { 28, 13,  46, 32,  47}, { 28, 40,  47,  7,  48},
        {28, 18,  47, 31,  48},
    },
    -- Q: 25%
    [3] = {
        {13,  1,  13,  0,  0},  { 22,  1,  22,  0,  0},  { 18,  2,  17,  0,  0},
        {26,  2,  24,  0,  0},  { 18,  2,  15,  2,  16}, { 24,  4,  19,  0,  0},
        {18,  2,  14,  4,  15}, { 22,  4,  18,  2,  19}, { 20,  4,  16,  4,  17},
        {24,  6,  19,  2,  20}, { 28,  4,  22,  4,  23}, { 26,  4,  20,  6,  21},
        {24,  8,  20,  4,  21}, { 20, 11,  16,  5,  17}, { 30,  5,  24,  7,  25},
        {24, 15,  19,  2,  20}, { 28,  1,  22, 15,  23}, { 28, 17,  22,  1,  23},
        {26, 17,  21,  4,  22}, { 30, 15,  24,  5,  25}, { 28, 17,  22,  6,  23},
        {30,  7,  24, 16,  25}, { 30, 11,  24, 14,  25}, { 30, 11,  24, 16,  25},
        {30,  7,  24, 22,  25}, { 28, 28,  22,  6,  23}, { 30,  8,  23, 26,  24},
        {30,  4,  24, 31,  25}, { 30,  1,  23, 37,  24}, { 30, 15,  24, 25,  25},
        {30, 42,  24,  1,  25}, { 30, 10,  24, 35,  25}, { 30, 29,  24, 19,  25},
        {30, 44,  24,  7,  25}, { 30, 39,  24, 14,  25}, { 30, 46,  24, 10,  25},
        {30, 49,  24, 10,  25}, { 30, 48,  24, 14,  25}, { 30, 43,  24, 22,  25},
        {30, 34,  24, 34,  25},
    },
    -- H: 30%
    [4] = {
        {17,  1,   9,  0,  0},  { 28,  1,  16,  0,  0},  { 22,  2,  13,  0,  0},
        {16,  4,   9,  0,  0},  { 22,  2,  11,  2,  12}, { 28,  4,  15,  0,  0},
        {26,  4,  13,  1,  14}, { 26,  4,  14,  2,  15}, { 24,  4,  12,  4,  13},
        {28,  6,  15,  2,  16}, { 24,  3,  12,  8,  13}, { 28,  7,  14,  4,  15},
        {22, 12,  11,  4,  12}, { 24, 11,  12,  5,  13}, { 24, 11,  12,  7,  13},
        {30,  3,  15, 13,  16}, { 28,  2,  14, 17,  15}, { 28,  2,  14, 19,  15},
        {26,  9,  13, 16,  14}, { 28, 15,  15, 10,  16}, { 30, 19,  16,  6,  17},
        {24, 34,  13,  0,  0},  { 30, 16,  15, 14,  16}, { 30, 30,  16,  2,  17},
        {30, 22,  15, 13,  16}, { 30, 33,  16,  4,  17}, { 30, 12,  15, 28,  16},
        {30, 11,  15, 31,  16}, { 30, 19,  15, 26,  16}, { 30, 23,  15, 25,  16},
        {30, 23,  15, 28,  16}, { 30, 19,  15, 35,  16}, { 30, 11,  15, 46,  16},
        {30, 59,  16,  1,  17}, { 30, 22,  15, 41,  16}, { 30,  2,  15, 64,  16},
        {30, 24,  15, 46,  16}, { 30, 42,  15, 32,  16}, { 30, 10,  15, 67,  16},
        {30, 20,  15, 61,  16},
    },
}

-- Alignment-pattern centre coords per version (Annex E). Versions
-- where the array has N entries → N×N possible positions (excluding
-- those overlapping finders). v1 has none.
local ALIGNMENT_POS = {
    {},
    {6, 18}, {6, 22}, {6, 26}, {6, 30}, {6, 34},
    {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50},
    {6, 30, 54}, {6, 32, 58}, {6, 34, 62},
    {6, 26, 46, 66}, {6, 26, 48, 70},
    {6, 26, 50, 74}, {6, 30, 54, 78}, {6, 30, 56, 82},
    {6, 30, 58, 86}, {6, 34, 62, 90},
    {6, 28, 50, 72, 94}, {6, 26, 50, 74, 98},
    {6, 30, 54, 78, 102}, {6, 28, 54, 80, 106},
    {6, 32, 58, 84, 110}, {6, 30, 58, 86, 114},
    {6, 34, 62, 90, 118},
    {6, 26, 50, 74, 98, 122}, {6, 30, 54, 78, 102, 126},
    {6, 26, 52, 78, 104, 130},
    {6, 30, 56, 82, 108, 134}, {6, 34, 60, 86, 112, 138},
    {6, 30, 58, 86, 114, 142}, {6, 34, 62, 90, 118, 146},
    {6, 30, 54, 78, 102, 126, 150},
    {6, 24, 50, 76, 102, 128, 154},
    {6, 28, 54, 80, 106, 132, 158},
    {6, 32, 58, 84, 110, 136, 162},
    {6, 26, 54, 82, 110, 138, 166},
    {6, 30, 58, 86, 114, 142, 170},
}
-- stylua: ignore end

local EC_LEVELS = { L = 1, M = 2, Q = 3, H = 4 }

-- Wire-format EC bits (placed inside the format info), per spec:
--   L=01, M=00, Q=11, H=10.
local EC_FORMAT_BITS = { [1] = 1, [2] = 0, [3] = 3, [4] = 2 }

-- ── GF(256) arithmetic for Reed-Solomon ────────────────────────────

local EXP, LOG = {}, {}
do
    local x = 1
    for i = 0, 254 do
        EXP[i] = x
        LOG[x] = i
        x = x << 1
        if x >= 256 then x = x ~ 0x11D end
    end
end

local function gf_mul(a, b)
    if a == 0 or b == 0 then return 0 end
    return EXP[(LOG[a] + LOG[b]) % 255]
end

-- Reed-Solomon generator polynomial g(x) = prod(i=0..n-1) (x - alpha^i)
-- Returned as `degree+1` coefficients, MSB first (g[0] is x^degree).
local function rs_generator(degree)
    -- Start with g(x) = 1 (constant polynomial, single coeff).
    local g = {}
    for i = 0, degree - 1 do g[i] = 0 end
    g[degree - 1] = 1  -- The constant 1, placed as the lowest coeff
    -- Use Nayuki-style: build polynomial of length `degree` representing
    -- generator's lower coeffs. Multiply by (x - alpha^i) iteratively.
    -- The product is g(x) of degree `degree`; we keep only the lower
    -- `degree` coefficients (the leading coeff is always 1).
    local root = 1
    for _ = 0, degree - 1 do
        for j = 0, degree - 1 do
            g[j] = gf_mul(g[j], root)
            if j + 1 < degree then
                g[j] = g[j] ~ g[j + 1]
            end
        end
        root = gf_mul(root, 2)  -- 2 is alpha in this field
    end
    return g
end

-- Compute Reed-Solomon EC codewords for `data` (list, 0-indexed) using
-- generator `gen` (0-indexed length = degree). Returns list of `degree`
-- bytes, 0-indexed.
local function rs_remainder(data, n_data, gen, degree)
    local rem = {}
    for i = 0, degree - 1 do rem[i] = 0 end
    for i = 0, n_data - 1 do
        local factor = data[i] ~ rem[0]
        for j = 0, degree - 2 do rem[j] = rem[j + 1] end
        rem[degree - 1] = 0
        for j = 0, degree - 1 do
            rem[j] = rem[j] ~ gf_mul(gen[j], factor)
        end
    end
    return rem
end

-- ── Capacity + version selection ───────────────────────────────────

local function data_capacity(version, ec)
    local row = EC_TABLE[ec][version]
    return row[2] * row[3] + row[4] * row[5]
end

local function pick_version(byte_len, ec)
    for v = 1, 40 do
        local count_bits = (v <= 9) and 8 or 16
        local need_bits = 4 + count_bits + 8 * byte_len
        local need_bytes = (need_bits + 7) // 8
        if need_bytes <= data_capacity(v, ec) then return v end
    end
    return nil
end

-- ── Data encoding ──────────────────────────────────────────────────

-- Encode `text` into a list of data codewords (0-indexed) of length
-- data_capacity(version, ec).
local function encode_payload(text, version, ec)
    local cap = data_capacity(version, ec)
    local out = {}
    for i = 0, cap - 1 do out[i] = 0 end

    -- Build the bit stream first, then pack.
    local bit_buf = {}  -- each element is a bit (0/1)
    local function put(value, width)
        for i = width - 1, 0, -1 do
            bit_buf[#bit_buf + 1] = (value >> i) & 1
        end
    end

    put(0x4, 4)                                  -- byte mode indicator
    put(#text, (version <= 9) and 8 or 16)       -- char count
    for i = 1, #text do put(string.byte(text, i), 8) end

    local cap_bits = cap * 8
    -- Terminator: up to 4 zero bits but only as many as fit.
    local term = math.min(4, cap_bits - #bit_buf)
    for _ = 1, term do bit_buf[#bit_buf + 1] = 0 end
    -- Pad to byte boundary.
    while (#bit_buf % 8) ~= 0 do bit_buf[#bit_buf + 1] = 0 end
    -- Pack bits into bytes.
    local n_bytes = #bit_buf // 8
    for i = 0, n_bytes - 1 do
        local b = 0
        for k = 0, 7 do b = (b << 1) | bit_buf[i * 8 + k + 1] end
        out[i] = b
    end
    -- Fill remainder with pad codewords 0xEC, 0x11 alternating.
    local pad = { 0xEC, 0x11 }
    local i = n_bytes
    while i < cap do
        out[i] = pad[((i - n_bytes) % 2) + 1]
        i = i + 1
    end
    return out
end

-- Interleave data + EC codewords per spec 8.6. Returns 0-indexed list.
local function build_codeword_stream(data, version, ec)
    local row = EC_TABLE[ec][version]
    local ec_per_block, g1_blocks, g1_data, g2_blocks, g2_data =
        row[1], row[2], row[3], row[4], row[5]

    -- Slice data into blocks (each block is a 0-indexed list).
    local blocks = {}
    local off = 0
    for _ = 1, g1_blocks do
        local b = {}
        for j = 0, g1_data - 1 do b[j] = data[off + j] end
        blocks[#blocks + 1] = { data = b, n = g1_data }
        off = off + g1_data
    end
    for _ = 1, g2_blocks do
        local b = {}
        for j = 0, g2_data - 1 do b[j] = data[off + j] end
        blocks[#blocks + 1] = { data = b, n = g2_data }
        off = off + g2_data
    end

    -- Compute EC codewords per block.
    local gen = rs_generator(ec_per_block)
    local ec_blocks = {}
    for i = 1, #blocks do
        ec_blocks[i] = rs_remainder(blocks[i].data, blocks[i].n,
                                     gen, ec_per_block)
    end

    -- Interleave: column 1 of every data block, then col 2, ...
    -- (skipping shorter blocks past their last index).
    local out = {}
    local idx = 0
    local max_data = (g2_data > 0) and g2_data or g1_data
    for col = 0, max_data - 1 do
        for i = 1, #blocks do
            if col < blocks[i].n then
                out[idx] = blocks[i].data[col]
                idx = idx + 1
            end
        end
    end
    -- Then column 1 of every EC block, col 2, ...
    for col = 0, ec_per_block - 1 do
        for i = 1, #blocks do
            out[idx] = ec_blocks[i][col]
            idx = idx + 1
        end
    end
    return out, idx
end

-- ── Matrix ─────────────────────────────────────────────────────────
--
-- We use a 0-indexed module grid internally to mirror the spec. The
-- public matrix returned by encode() is 1-indexed to match Lua
-- conventions and existing Hull stdlib style.

local function new_grid(size)
    local g, f = {}, {}
    for r = 0, size - 1 do
        g[r] = {}; f[r] = {}
        for c = 0, size - 1 do g[r][c] = 0; f[r][c] = false end
    end
    return g, f
end

local function set_finder(g, f, top_r, top_c)
    -- 7x7 finder, then 1-module separator white border (only inside).
    for dr = -1, 7 do
        for dc = -1, 7 do
            local r = top_r + dr
            local c = top_c + dc
            if g[r] and g[r][c] ~= nil then
                local dark = false
                if dr >= 0 and dr <= 6 and dc >= 0 and dc <= 6 then
                    -- Outer ring or inner 3x3.
                    if dr == 0 or dr == 6 or dc == 0 or dc == 6
                       or (dr >= 2 and dr <= 4 and dc >= 2 and dc <= 4) then
                        dark = true
                    end
                end
                g[r][c] = dark and 1 or 0
                f[r][c] = true
            end
        end
    end
end

local function set_alignment(g, f, cr, cc)
    for dr = -2, 2 do
        for dc = -2, 2 do
            local dark = math.abs(dr) == 2 or math.abs(dc) == 2
                          or (dr == 0 and dc == 0)
            g[cr + dr][cc + dc] = dark and 1 or 0
            f[cr + dr][cc + dc] = true
        end
    end
end

local function draw_function_patterns(g, f, version, size)
    -- Horizontal + vertical timing patterns on row 6 / col 6 (skipping
    -- the finder areas). Drawn first so finders overwrite the corners.
    for i = 0, size - 1 do
        g[6][i] = ((i % 2) == 0) and 1 or 0
        g[i][6] = ((i % 2) == 0) and 1 or 0
        f[6][i] = true
        f[i][6] = true
    end

    -- Three finder patterns + separators.
    set_finder(g, f, 0, 0)
    set_finder(g, f, 0, size - 7)
    set_finder(g, f, size - 7, 0)

    -- Reserve format info area around the finders (15 bits, both copies).
    for i = 0, 8 do
        f[8][i] = true     -- bottom of top-left
        f[i][8] = true     -- right of top-left
    end
    for i = 0, 7 do
        f[size - 1 - i][8] = true  -- bottom-left vertical
        f[8][size - 1 - i] = true  -- top-right horizontal
    end

    -- Alignment patterns (v2+); skip those overlapping finders.
    local pos = ALIGNMENT_POS[version]
    local n = #pos
    for i = 1, n do
        for j = 1, n do
            local skip = (i == 1 and j == 1)
                      or (i == 1 and j == n)
                      or (i == n and j == 1)
            if not skip then
                set_alignment(g, f, pos[i], pos[j])
            end
        end
    end

    -- Dark module - always 1 at (4V+9, 8).
    g[4 * version + 9][8] = 1
    f[4 * version + 9][8] = true

    -- Reserve version-info area (v7+).
    if version >= 7 then
        for i = 0, 5 do
            for j = 0, 2 do
                f[size - 11 + j][i] = true
                f[i][size - 11 + j] = true
            end
        end
    end
end

-- Place codeword stream into the grid using the spec's zigzag scan.
local function place_codewords(g, f, size, stream, n_bytes)
    local bit_idx = 0
    local total = n_bytes * 8
    local right = size - 1
    while right >= 1 do
        if right == 6 then right = 5 end  -- skip the vertical timing col
        for vert = 0, size - 1 do
            for j = 0, 1 do
                local x = right - j
                local upward = (((right + 1) & 2) == 0)
                local y = upward and (size - 1 - vert) or vert
                if not f[y][x] then
                    if bit_idx < total then
                        local byte = stream[bit_idx >> 3]
                        local bit  = (byte >> (7 - (bit_idx & 7))) & 1
                        g[y][x] = bit
                    end
                    bit_idx = bit_idx + 1
                end
            end
        end
        right = right - 2
    end
end

-- ── Masking ────────────────────────────────────────────────────────

local function mask_at(mask, r, c)
    if mask == 0 then return ((r + c) % 2) == 0
    elseif mask == 1 then return (r % 2) == 0
    elseif mask == 2 then return (c % 3) == 0
    elseif mask == 3 then return ((r + c) % 3) == 0
    elseif mask == 4 then return (((r >> 1) + (c // 3)) % 2) == 0
    elseif mask == 5 then return ((r * c) % 2 + (r * c) % 3) == 0
    elseif mask == 6 then return (((r * c) % 2 + (r * c) % 3) % 2) == 0
    else                return ((((r + c) % 2) + ((r * c) % 3)) % 2) == 0
    end
end

local function apply_mask(g, f, size, mask)
    for r = 0, size - 1 do
        for c = 0, size - 1 do
            if not f[r][c] and mask_at(mask, r, c) then
                g[r][c] = g[r][c] ~ 1
            end
        end
    end
end

local function score(g, size)
    local penalty = 0
    -- Rule 1: ≥5 same-color runs in rows + cols.
    for r = 0, size - 1 do
        local run, last = 0, -1
        for c = 0, size - 1 do
            if g[r][c] == last then
                run = run + 1
                if run == 5 then penalty = penalty + 3
                elseif run > 5 then penalty = penalty + 1 end
            else
                last = g[r][c]; run = 1
            end
        end
    end
    for c = 0, size - 1 do
        local run, last = 0, -1
        for r = 0, size - 1 do
            if g[r][c] == last then
                run = run + 1
                if run == 5 then penalty = penalty + 3
                elseif run > 5 then penalty = penalty + 1 end
            else
                last = g[r][c]; run = 1
            end
        end
    end
    -- Rule 2: 2x2 same-color blocks.
    for r = 0, size - 2 do
        for c = 0, size - 2 do
            local v = g[r][c]
            if v == g[r][c + 1] and v == g[r + 1][c] and v == g[r + 1][c + 1] then
                penalty = penalty + 3
            end
        end
    end
    -- Rule 3: finder-like patterns. Look for 1011101 with 4-light on
    -- either side, in rows + cols.
    local function fline(get_cell)
        local pat_l = {0,0,0,0,1,0,1,1,1,0,1}
        local pat_r = {1,0,1,1,1,0,1,0,0,0,0}
        local pen = 0
        for i = 0, size - 11 do
            local ml, mr = true, true
            for k = 1, 11 do
                local v = get_cell(i + k - 1)
                if v ~= pat_l[k] then ml = false end
                if v ~= pat_r[k] then mr = false end
            end
            if ml then pen = pen + 40 end
            if mr then pen = pen + 40 end
        end
        return pen
    end
    for r = 0, size - 1 do
        penalty = penalty + fline(function(i) return g[r][i] end)
    end
    for c = 0, size - 1 do
        penalty = penalty + fline(function(i) return g[i][c] end)
    end
    -- Rule 4: dark-pixel proportion deviation from 50%, in 5% steps.
    -- Penalty = 10 * floor(|dark_percent - 50| / 5).
    local dark = 0
    for r = 0, size - 1 do for c = 0, size - 1 do dark = dark + g[r][c] end end
    local pct = (dark * 100) // (size * size)
    penalty = penalty + (math.abs(pct - 50) // 5) * 10
    return penalty
end

-- ── Format info + version info ─────────────────────────────────────

-- BCH(15,5) over GF(2) with generator x^10+x^8+x^5+x^4+x^2+x+1 (0x537).
local function format_info_bits(ec_internal, mask)
    local data = (EC_FORMAT_BITS[ec_internal] << 3) | mask
    local rem = data
    for _ = 1, 10 do
        rem = (rem << 1) ~ ((rem >> 9) * 0x537)
    end
    local bits = ((data << 10) | (rem & 0x3FF)) ~ 0x5412
    return bits  -- 15-bit value
end

local function place_format_info(g, ec_internal, mask, size)
    local bits = format_info_bits(ec_internal, mask)
    -- get_bit(i): bit i (LSB-indexed) of the 15-bit format-info word.
    local function bit(i) return (bits >> i) & 1 end

    -- Copy 1: around top-left finder.
    -- Bits 0-5 → col 8, rows 0-5 (top down).
    for i = 0, 5 do g[i][8] = bit(i) end
    g[7][8] = bit(6)   -- skip row 6 (timing) - bit 6 goes to row 7
    g[8][8] = bit(7)
    g[8][7] = bit(8)   -- skip col 6 (timing) - bit 8 goes to col 7
    -- Bits 9-14 → row 8, cols 5..0 (right to left).
    for i = 9, 14 do g[8][14 - i] = bit(i) end

    -- Copy 2: top-right (bits 0-7 along row 8, RTL just left of top-
    -- right finder) + bottom-left (bits 8-14 down col 8 just below
    -- bottom-left finder). Matches Nayuki's reference; I had row/col
    -- swapped earlier. The two halves go on the two OTHER finders.
    for i = 0, 7 do g[8][size - 1 - i] = bit(i) end
    for i = 8, 14 do g[size - 15 + i][8] = bit(i) end

    -- Dark module is reapplied (some impls do this here; harmless).
end

-- BCH(18,6) for version info, generator x^12+x^11+x^10+x^9+x^8+x^5+x^2+1 (0x1F25).
local function version_info_bits(version)
    local rem = version
    for _ = 1, 12 do
        rem = (rem << 1) ~ ((rem >> 11) * 0x1F25)
    end
    return (version << 12) | (rem & 0xFFF)  -- 18-bit value
end

local function place_version_info(g, version, size)
    if version < 7 then return end
    local bits = version_info_bits(version)
    for i = 0, 17 do
        local bit = (bits >> i) & 1
        local a, b = i // 3, i % 3
        local x = size - 11 + b
        local y = a
        g[y][x] = bit
        g[x][y] = bit
    end
end

-- ── Public API ─────────────────────────────────────────────────────

--- Encode text into a QR code matrix.
function M.encode(text, opts)
    opts = opts or {}
    local ec_name = opts.ec_level or "M"
    local ec = EC_LEVELS[ec_name]
    if not ec then error("qrcode.encode: invalid ec_level '" .. tostring(ec_name) .. "'") end

    local version = pick_version(#text, ec)
    if not version then
        error("qrcode.encode: payload too large for any version at EC " .. ec_name)
    end

    local data = encode_payload(text, version, ec)
    local stream, n_bytes = build_codeword_stream(data, version, ec)
    local size = 17 + 4 * version
    local g, f = new_grid(size)
    draw_function_patterns(g, f, version, size)
    place_codewords(g, f, size, stream, n_bytes)
    place_version_info(g, version, size)

    -- Pick mask. Either honor the explicit choice or run all 8 + score.
    local chosen = opts.mask
    if chosen == nil then
        local best_pen = math.huge
        for m = 0, 7 do
            apply_mask(g, f, size, m)
            place_format_info(g, ec, m, size)
            local pen = score(g, size)
            if pen < best_pen then best_pen, chosen = pen, m end
            apply_mask(g, f, size, m)  -- toggle off
        end
    end
    apply_mask(g, f, size, chosen)
    place_format_info(g, ec, chosen, size)

    -- Convert grid to 1-indexed for the public matrix.
    local matrix = {}
    for r = 1, size do
        matrix[r] = {}
        for c = 1, size do matrix[r][c] = g[r - 1][c - 1] end
    end

    return {
        matrix = matrix,
        size = size,
        version = version,
        ec_level = ec_name,
        mask = chosen,
    }
end

--- Encode `text` and serialize as an SVG string.
function M.svg(text, opts)
    opts = opts or {}
    local q = M.encode(text, opts)
    local scale = opts.scale or 4
    local margin = opts.margin or 4
    local dark = opts.dark or "#000"
    local light = opts.light or "#fff"
    -- Both colors are interpolated raw into SVG attributes below.
    -- Reject anything that isn't a plain color literal so an app
    -- that ever wires user input (e.g. `opts.dark = req.query.color`)
    -- can't break out into arbitrary XML / script. Accepts CSS hex
    -- (`#fff`, `#ffffff`), bare named colors (`red`, `white`), the
    -- literal `none`, and rgb() / rgba() function notation.
    local function valid_color(s)
        if type(s) ~= "string" or #s == 0 or #s > 32 then return false end
        if s == "none" then return true end
        if s:match("^#?[%x]+$") then return true end
        if s:match("^[A-Za-z]+$") then return true end
        if s:match("^rgba?%(%s*[%d%s,%.]+%s*%)$") then return true end
        return false
    end
    if not valid_color(dark) then
        error("qrcode.svg: invalid opts.dark color '"
              .. tostring(dark) .. "'")
    end
    if not valid_color(light) then
        error("qrcode.svg: invalid opts.light color '"
              .. tostring(light) .. "'")
    end
    local full = (q.size + 2 * margin) * scale

    local parts = {}
    parts[#parts + 1] = string.format(
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" shape-rendering="crispEdges">',
        full, full, full, full)
    if light ~= "none" then
        parts[#parts + 1] = string.format(
            '<rect width="%d" height="%d" fill="%s"/>', full, full, light)
    end
    local segs = {}
    for r = 1, q.size do
        for c = 1, q.size do
            if q.matrix[r][c] == 1 then
                local x = (margin + c - 1) * scale
                local y = (margin + r - 1) * scale
                segs[#segs + 1] = string.format("M%d %dh%dv%dh-%dz",
                    x, y, scale, scale, scale)
            end
        end
    end
    parts[#parts + 1] = string.format(
        '<path fill="%s" d="%s"/>', dark, table.concat(segs))
    parts[#parts + 1] = "</svg>"
    return table.concat(parts)
end

return M
