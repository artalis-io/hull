/**
 * @file hull:qrcode
 * @module hull:qrcode
 * @description QR Code generator (ISO/IEC 18004).
 *
 * Lua parity: same surface as `hull.qrcode`. Byte mode only, all
 * four EC levels (L 7%, M 15%, Q 25%, H 30%), versions 1-40, all
 * 8 mask patterns scored per spec section 8.8.2.
 *
 * @license AGPL-3.0-or-later
 *
 * Algorithm structure adapted from Project Nayuki's "QR Code
 * generator library" (MIT-licensed,
 * https://www.nayuki.io/page/qr-code-generator-library). Tables
 * transcribed from ISO/IEC 18004:2015 Annex E + Table 9. All wire-
 * level math is in 0-indexed coordinates (matching the spec).
 *
 * @example
 *   import { qrcode } from "hull:qrcode";
 *   const q = qrcode.encode("Hello", { ecLevel: "M" });
 *   // q.matrix (2D array, 1-indexed), q.size, q.version, q.mask
 *   const svg = qrcode.svg("https://example.com", { scale: 6 });
 */

// ── EC block layout (ISO/IEC 18004 Table 9) ─────────────────────────
// Per (ec, version) tuple: [ec_per_block, g1_blocks, g1_data, g2_blocks, g2_data]
// EC ints: 1=L, 2=M, 3=Q, 4=H.

const EC_TABLE = {
    1: [  // L: 7%
        [ 7,  1,  19,  0,  0], [10,  1,  34,  0,  0], [15,  1,  55,  0,  0],
        [20,  1,  80,  0,  0], [26,  1, 108,  0,  0], [18,  2,  68,  0,  0],
        [20,  2,  78,  0,  0], [24,  2,  97,  0,  0], [30,  2, 116,  0,  0],
        [18,  2,  68,  2,  69], [20,  4,  81,  0,  0], [24,  2,  92,  2,  93],
        [26,  4, 107,  0,  0], [30,  3, 115,  1, 116], [22,  5,  87,  1,  88],
        [24,  5,  98,  1,  99], [28,  1, 107,  5, 108], [30,  5, 120,  1, 121],
        [28,  3, 113,  4, 114], [28,  3, 107,  5, 108], [28,  4, 116,  4, 117],
        [28,  2, 111,  7, 112], [30,  4, 121,  5, 122], [30,  6, 117,  4, 118],
        [26,  8, 106,  4, 107], [28, 10, 114,  2, 115], [30,  8, 122,  4, 123],
        [30,  3, 117, 10, 118], [30,  7, 116,  7, 117], [30,  5, 115, 10, 116],
        [30, 13, 115,  3, 116], [30, 17, 115,  0,  0], [30, 17, 115,  1, 116],
        [30, 13, 115,  6, 116], [30, 12, 121,  7, 122], [30,  6, 121, 14, 122],
        [30, 17, 122,  4, 123], [30,  4, 122, 18, 123], [30, 20, 117,  4, 118],
        [30, 19, 118,  6, 119],
    ],
    2: [  // M: 15%
        [10,  1,  16,  0,  0], [16,  1,  28,  0,  0], [26,  1,  44,  0,  0],
        [18,  2,  32,  0,  0], [24,  2,  43,  0,  0], [16,  4,  27,  0,  0],
        [18,  4,  31,  0,  0], [22,  2,  38,  2,  39], [22,  3,  36,  2,  37],
        [26,  4,  43,  1,  44], [30,  1,  50,  4,  51], [22,  6,  36,  2,  37],
        [22,  8,  37,  1,  38], [24,  4,  40,  5,  41], [24,  5,  41,  5,  42],
        [28,  7,  45,  3,  46], [28, 10,  46,  1,  47], [26,  9,  43,  4,  44],
        [26,  3,  44, 11,  45], [26,  3,  41, 13,  42], [26, 17,  42,  0,  0],
        [28, 17,  46,  0,  0], [28,  4,  47, 14,  48], [28,  6,  45, 14,  46],
        [28,  8,  47, 13,  48], [28, 19,  46,  4,  47], [28, 22,  45,  3,  46],
        [28,  3,  45, 23,  46], [28, 21,  45,  7,  46], [28, 19,  47, 10,  48],
        [28,  2,  46, 29,  47], [28, 10,  46, 23,  47], [28, 14,  46, 21,  47],
        [28, 14,  46, 23,  47], [28, 12,  47, 26,  48], [28,  6,  47, 34,  48],
        [28, 29,  46, 14,  47], [28, 13,  46, 32,  47], [28, 40,  47,  7,  48],
        [28, 18,  47, 31,  48],
    ],
    3: [  // Q: 25%
        [13,  1,  13,  0,  0], [22,  1,  22,  0,  0], [18,  2,  17,  0,  0],
        [26,  2,  24,  0,  0], [18,  2,  15,  2,  16], [24,  4,  19,  0,  0],
        [18,  2,  14,  4,  15], [22,  4,  18,  2,  19], [20,  4,  16,  4,  17],
        [24,  6,  19,  2,  20], [28,  4,  22,  4,  23], [26,  4,  20,  6,  21],
        [24,  8,  20,  4,  21], [20, 11,  16,  5,  17], [30,  5,  24,  7,  25],
        [24, 15,  19,  2,  20], [28,  1,  22, 15,  23], [28, 17,  22,  1,  23],
        [26, 17,  21,  4,  22], [30, 15,  24,  5,  25], [28, 17,  22,  6,  23],
        [30,  7,  24, 16,  25], [30, 11,  24, 14,  25], [30, 11,  24, 16,  25],
        [30,  7,  24, 22,  25], [28, 28,  22,  6,  23], [30,  8,  23, 26,  24],
        [30,  4,  24, 31,  25], [30,  1,  23, 37,  24], [30, 15,  24, 25,  25],
        [30, 42,  24,  1,  25], [30, 10,  24, 35,  25], [30, 29,  24, 19,  25],
        [30, 44,  24,  7,  25], [30, 39,  24, 14,  25], [30, 46,  24, 10,  25],
        [30, 49,  24, 10,  25], [30, 48,  24, 14,  25], [30, 43,  24, 22,  25],
        [30, 34,  24, 34,  25],
    ],
    4: [  // H: 30%
        [17,  1,   9,  0,  0], [28,  1,  16,  0,  0], [22,  2,  13,  0,  0],
        [16,  4,   9,  0,  0], [22,  2,  11,  2,  12], [28,  4,  15,  0,  0],
        [26,  4,  13,  1,  14], [26,  4,  14,  2,  15], [24,  4,  12,  4,  13],
        [28,  6,  15,  2,  16], [24,  3,  12,  8,  13], [28,  7,  14,  4,  15],
        [22, 12,  11,  4,  12], [24, 11,  12,  5,  13], [24, 11,  12,  7,  13],
        [30,  3,  15, 13,  16], [28,  2,  14, 17,  15], [28,  2,  14, 19,  15],
        [26,  9,  13, 16,  14], [28, 15,  15, 10,  16], [30, 19,  16,  6,  17],
        [24, 34,  13,  0,  0], [30, 16,  15, 14,  16], [30, 30,  16,  2,  17],
        [30, 22,  15, 13,  16], [30, 33,  16,  4,  17], [30, 12,  15, 28,  16],
        [30, 11,  15, 31,  16], [30, 19,  15, 26,  16], [30, 23,  15, 25,  16],
        [30, 23,  15, 28,  16], [30, 19,  15, 35,  16], [30, 11,  15, 46,  16],
        [30, 59,  16,  1,  17], [30, 22,  15, 41,  16], [30,  2,  15, 64,  16],
        [30, 24,  15, 46,  16], [30, 42,  15, 32,  16], [30, 10,  15, 67,  16],
        [30, 20,  15, 61,  16],
    ],
};

const ALIGNMENT_POS = [
    [],
    [6, 18], [6, 22], [6, 26], [6, 30], [6, 34],
    [6, 22, 38], [6, 24, 42], [6, 26, 46], [6, 28, 50],
    [6, 30, 54], [6, 32, 58], [6, 34, 62],
    [6, 26, 46, 66], [6, 26, 48, 70],
    [6, 26, 50, 74], [6, 30, 54, 78], [6, 30, 56, 82],
    [6, 30, 58, 86], [6, 34, 62, 90],
    [6, 28, 50, 72, 94], [6, 26, 50, 74, 98],
    [6, 30, 54, 78, 102], [6, 28, 54, 80, 106],
    [6, 32, 58, 84, 110], [6, 30, 58, 86, 114],
    [6, 34, 62, 90, 118],
    [6, 26, 50, 74, 98, 122], [6, 30, 54, 78, 102, 126],
    [6, 26, 52, 78, 104, 130],
    [6, 30, 56, 82, 108, 134], [6, 34, 60, 86, 112, 138],
    [6, 30, 58, 86, 114, 142], [6, 34, 62, 90, 118, 146],
    [6, 30, 54, 78, 102, 126, 150],
    [6, 24, 50, 76, 102, 128, 154],
    [6, 28, 54, 80, 106, 132, 158],
    [6, 32, 58, 84, 110, 136, 162],
    [6, 26, 54, 82, 110, 138, 166],
    [6, 30, 58, 86, 114, 142, 170],
];

const EC_LEVELS = { L: 1, M: 2, Q: 3, H: 4 };
const EC_FORMAT_BITS = { 1: 1, 2: 0, 3: 3, 4: 2 };  // L=01 M=00 Q=11 H=10

// ── GF(256) arithmetic ─────────────────────────────────────────────

const EXP = new Uint8Array(255);
const LOG = new Uint8Array(256);
{
    let x = 1;
    for (let i = 0; i < 255; i++) {
        EXP[i] = x;
        LOG[x] = i;
        x <<= 1;
        if (x >= 256) x ^= 0x11D;
    }
}

function gfMul(a, b) {
    if (a === 0 || b === 0) return 0;
    return EXP[(LOG[a] + LOG[b]) % 255];
}

function rsGenerator(degree) {
    const g = new Uint8Array(degree);
    g[degree - 1] = 1;
    let root = 1;
    for (let i = 0; i < degree; i++) {
        for (let j = 0; j < degree; j++) {
            g[j] = gfMul(g[j], root);
            if (j + 1 < degree) g[j] ^= g[j + 1];
        }
        root = gfMul(root, 2);
    }
    return g;
}

function rsRemainder(data, nData, gen, degree) {
    const rem = new Uint8Array(degree);
    for (let i = 0; i < nData; i++) {
        const factor = data[i] ^ rem[0];
        for (let j = 0; j < degree - 1; j++) rem[j] = rem[j + 1];
        rem[degree - 1] = 0;
        for (let j = 0; j < degree; j++) rem[j] ^= gfMul(gen[j], factor);
    }
    return rem;
}

// ── Capacity + version pick ────────────────────────────────────────

function dataCapacity(version, ec) {
    const row = EC_TABLE[ec][version - 1];
    return row[1] * row[2] + row[3] * row[4];
}

function pickVersion(byteLen, ec) {
    for (let v = 1; v <= 40; v++) {
        const countBits = v <= 9 ? 8 : 16;
        const needBytes = Math.ceil((4 + countBits + 8 * byteLen) / 8);
        if (needBytes <= dataCapacity(v, ec)) return v;
    }
    return null;
}

// ── Encoding ───────────────────────────────────────────────────────

function encodePayload(text, version, ec) {
    const cap = dataCapacity(version, ec);
    const out = new Uint8Array(cap);
    const bitBuf = [];
    const put = (value, width) => {
        for (let i = width - 1; i >= 0; i--) bitBuf.push((value >> i) & 1);
    };

    put(0x4, 4);
    put(text.length, version <= 9 ? 8 : 16);
    for (let i = 0; i < text.length; i++) put(text.charCodeAt(i) & 0xff, 8);

    const capBits = cap * 8;
    const term = Math.min(4, capBits - bitBuf.length);
    for (let i = 0; i < term; i++) bitBuf.push(0);
    while (bitBuf.length % 8 !== 0) bitBuf.push(0);

    const nBytes = bitBuf.length / 8;
    for (let i = 0; i < nBytes; i++) {
        let b = 0;
        for (let k = 0; k < 8; k++) b = (b << 1) | bitBuf[i * 8 + k];
        out[i] = b;
    }
    const pad = [0xEC, 0x11];
    for (let i = nBytes; i < cap; i++) out[i] = pad[(i - nBytes) % 2];
    return out;
}

function buildCodewordStream(data, version, ec) {
    const row = EC_TABLE[ec][version - 1];
    const [ecPerBlock, g1Blocks, g1Data, g2Blocks, g2Data] = row;

    const blocks = [];
    let off = 0;
    for (let i = 0; i < g1Blocks; i++) {
        blocks.push({ data: data.slice(off, off + g1Data), n: g1Data });
        off += g1Data;
    }
    for (let i = 0; i < g2Blocks; i++) {
        blocks.push({ data: data.slice(off, off + g2Data), n: g2Data });
        off += g2Data;
    }

    const gen = rsGenerator(ecPerBlock);
    const ecBlocks = blocks.map(b => rsRemainder(b.data, b.n, gen, ecPerBlock));

    const out = [];
    const maxData = g2Data > 0 ? g2Data : g1Data;
    for (let col = 0; col < maxData; col++) {
        for (let i = 0; i < blocks.length; i++) {
            if (col < blocks[i].n) out.push(blocks[i].data[col]);
        }
    }
    for (let col = 0; col < ecPerBlock; col++) {
        for (let i = 0; i < ecBlocks.length; i++) {
            out.push(ecBlocks[i][col]);
        }
    }
    return out;
}

// ── Matrix ─────────────────────────────────────────────────────────

function newGrid(size) {
    const g = new Array(size), f = new Array(size);
    for (let r = 0; r < size; r++) {
        g[r] = new Uint8Array(size);
        f[r] = new Array(size).fill(false);
    }
    return { g, f };
}

function setFinder(g, f, topR, topC) {
    for (let dr = -1; dr <= 7; dr++) {
        for (let dc = -1; dc <= 7; dc++) {
            const r = topR + dr, c = topC + dc;
            if (g[r] !== undefined && g[r][c] !== undefined) {
                let dark = false;
                if (dr >= 0 && dr <= 6 && dc >= 0 && dc <= 6) {
                    if (dr === 0 || dr === 6 || dc === 0 || dc === 6
                        || (dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4)) {
                        dark = true;
                    }
                }
                g[r][c] = dark ? 1 : 0;
                f[r][c] = true;
            }
        }
    }
}

function setAlignment(g, f, cr, cc) {
    for (let dr = -2; dr <= 2; dr++) {
        for (let dc = -2; dc <= 2; dc++) {
            const dark = Math.abs(dr) === 2 || Math.abs(dc) === 2
                          || (dr === 0 && dc === 0);
            g[cr + dr][cc + dc] = dark ? 1 : 0;
            f[cr + dr][cc + dc] = true;
        }
    }
}

function drawFunctionPatterns(g, f, version, size) {
    for (let i = 0; i < size; i++) {
        g[6][i] = (i % 2 === 0) ? 1 : 0;
        g[i][6] = (i % 2 === 0) ? 1 : 0;
        f[6][i] = true;
        f[i][6] = true;
    }
    setFinder(g, f, 0, 0);
    setFinder(g, f, 0, size - 7);
    setFinder(g, f, size - 7, 0);

    for (let i = 0; i <= 8; i++) {
        f[8][i] = true;
        f[i][8] = true;
    }
    for (let i = 0; i < 8; i++) {
        f[size - 1 - i][8] = true;
        f[8][size - 1 - i] = true;
    }

    const pos = ALIGNMENT_POS[version - 1];
    const n = pos.length;
    for (let i = 0; i < n; i++) {
        for (let j = 0; j < n; j++) {
            const skip = (i === 0 && j === 0)
                      || (i === 0 && j === n - 1)
                      || (i === n - 1 && j === 0);
            if (!skip) setAlignment(g, f, pos[i], pos[j]);
        }
    }

    g[4 * version + 9][8] = 1;
    f[4 * version + 9][8] = true;

    if (version >= 7) {
        for (let i = 0; i < 6; i++) {
            for (let j = 0; j < 3; j++) {
                f[size - 11 + j][i] = true;
                f[i][size - 11 + j] = true;
            }
        }
    }
}

function placeCodewords(g, f, size, stream) {
    let bitIdx = 0;
    const total = stream.length * 8;
    let right = size - 1;
    while (right >= 1) {
        if (right === 6) right = 5;
        for (let vert = 0; vert < size; vert++) {
            for (let j = 0; j < 2; j++) {
                const x = right - j;
                const upward = (((right + 1) & 2) === 0);
                const y = upward ? (size - 1 - vert) : vert;
                if (!f[y][x]) {
                    if (bitIdx < total) {
                        const byte = stream[bitIdx >> 3];
                        const bit = (byte >> (7 - (bitIdx & 7))) & 1;
                        g[y][x] = bit;
                    }
                    bitIdx++;
                }
            }
        }
        right -= 2;
    }
}

// ── Masking ────────────────────────────────────────────────────────

function maskAt(mask, r, c) {
    switch (mask) {
        case 0: return (r + c) % 2 === 0;
        case 1: return r % 2 === 0;
        case 2: return c % 3 === 0;
        case 3: return (r + c) % 3 === 0;
        case 4: return ((r >> 1) + Math.floor(c / 3)) % 2 === 0;
        case 5: return ((r * c) % 2 + (r * c) % 3) === 0;
        case 6: return (((r * c) % 2 + (r * c) % 3) % 2) === 0;
        default: return ((((r + c) % 2) + ((r * c) % 3)) % 2) === 0;
    }
}

function applyMask(g, f, size, mask) {
    for (let r = 0; r < size; r++) {
        for (let c = 0; c < size; c++) {
            if (!f[r][c] && maskAt(mask, r, c)) g[r][c] ^= 1;
        }
    }
}

function score(g, size) {
    let penalty = 0;
    // Rule 1
    for (let r = 0; r < size; r++) {
        let run = 0, last = -1;
        for (let c = 0; c < size; c++) {
            if (g[r][c] === last) {
                run++;
                if (run === 5) penalty += 3;
                else if (run > 5) penalty += 1;
            } else { last = g[r][c]; run = 1; }
        }
    }
    for (let c = 0; c < size; c++) {
        let run = 0, last = -1;
        for (let r = 0; r < size; r++) {
            if (g[r][c] === last) {
                run++;
                if (run === 5) penalty += 3;
                else if (run > 5) penalty += 1;
            } else { last = g[r][c]; run = 1; }
        }
    }
    // Rule 2
    for (let r = 0; r < size - 1; r++) {
        for (let c = 0; c < size - 1; c++) {
            const v = g[r][c];
            if (v === g[r][c+1] && v === g[r+1][c] && v === g[r+1][c+1]) {
                penalty += 3;
            }
        }
    }
    // Rule 3
    const patL = [0,0,0,0,1,0,1,1,1,0,1];
    const patR = [1,0,1,1,1,0,1,0,0,0,0];
    const checkLine = (get) => {
        let pen = 0;
        for (let i = 0; i <= size - 11; i++) {
            let ml = true, mr = true;
            for (let k = 0; k < 11; k++) {
                const v = get(i + k);
                if (v !== patL[k]) ml = false;
                if (v !== patR[k]) mr = false;
            }
            if (ml) pen += 40;
            if (mr) pen += 40;
        }
        return pen;
    };
    for (let r = 0; r < size; r++) penalty += checkLine(i => g[r][i]);
    for (let c = 0; c < size; c++) penalty += checkLine(i => g[i][c]);
    // Rule 4
    let dark = 0;
    for (let r = 0; r < size; r++) for (let c = 0; c < size; c++) dark += g[r][c];
    const pct = Math.floor((dark * 100) / (size * size));
    penalty += Math.floor(Math.abs(pct - 50) / 5) * 10;
    return penalty;
}

// ── Format + version info ──────────────────────────────────────────

function formatInfoBits(ecInternal, mask) {
    const data = (EC_FORMAT_BITS[ecInternal] << 3) | mask;
    let rem = data;
    for (let i = 0; i < 10; i++) {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    }
    return ((data << 10) | (rem & 0x3FF)) ^ 0x5412;
}

function placeFormatInfo(g, ecInternal, mask, size) {
    const bits = formatInfoBits(ecInternal, mask);
    const bit = i => (bits >> i) & 1;
    for (let i = 0; i <= 5; i++) g[i][8] = bit(i);
    g[7][8] = bit(6);
    g[8][8] = bit(7);
    g[8][7] = bit(8);
    for (let i = 9; i <= 14; i++) g[8][14 - i] = bit(i);
    for (let i = 0; i <= 7; i++) g[8][size - 1 - i] = bit(i);
    for (let i = 8; i <= 14; i++) g[size - 15 + i][8] = bit(i);
}

function versionInfoBits(version) {
    let rem = version;
    for (let i = 0; i < 12; i++) {
        rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    }
    return (version << 12) | (rem & 0xFFF);
}

function placeVersionInfo(g, version, size) {
    if (version < 7) return;
    const bits = versionInfoBits(version);
    for (let i = 0; i < 18; i++) {
        const b = (bits >> i) & 1;
        const a = Math.floor(i / 3), col = i % 3;
        g[a][size - 11 + col] = b;
        g[size - 11 + col][a] = b;
    }
}

// ── Public API ─────────────────────────────────────────────────────

function encode(text, opts) {
    opts = opts || {};
    const ecName = opts.ecLevel || "M";
    const ec = EC_LEVELS[ecName];
    if (!ec) throw new Error("qrcode.encode: invalid ecLevel '" + ecName + "'");

    const version = pickVersion(text.length, ec);
    if (!version) {
        throw new Error("qrcode.encode: payload too large for any version at EC " + ecName);
    }

    const data = encodePayload(text, version, ec);
    const stream = buildCodewordStream(data, version, ec);
    const size = 17 + 4 * version;
    const { g, f } = newGrid(size);
    drawFunctionPatterns(g, f, version, size);
    placeCodewords(g, f, size, stream);
    placeVersionInfo(g, version, size);

    let chosen = opts.mask;
    if (chosen === undefined || chosen === null) {
        let bestPen = Infinity;
        for (let m = 0; m < 8; m++) {
            applyMask(g, f, size, m);
            placeFormatInfo(g, ec, m, size);
            const pen = score(g, size);
            if (pen < bestPen) { bestPen = pen; chosen = m; }
            applyMask(g, f, size, m);
        }
    }
    applyMask(g, f, size, chosen);
    placeFormatInfo(g, ec, chosen, size);

    // Convert to 1-indexed matrix to match Lua public shape.
    const matrix = new Array(size);
    for (let r = 1; r <= size; r++) {
        matrix[r] = new Array(size + 1);
        for (let c = 1; c <= size; c++) matrix[r][c] = g[r - 1][c - 1];
    }
    return { matrix, size, version, ecLevel: ecName, mask: chosen };
}

function svg(text, opts) {
    opts = opts || {};
    const q = encode(text, opts);
    const scale = opts.scale || 4;
    const margin = opts.margin || 4;
    const dark = opts.dark || "#000";
    const light = opts.light || "#fff";
    const full = (q.size + 2 * margin) * scale;

    const parts = [];
    parts.push(
        `<svg xmlns="http://www.w3.org/2000/svg" width="${full}" height="${full}" viewBox="0 0 ${full} ${full}" shape-rendering="crispEdges">`);
    if (light !== "none") {
        parts.push(`<rect width="${full}" height="${full}" fill="${light}"/>`);
    }
    const segs = [];
    for (let r = 1; r <= q.size; r++) {
        for (let c = 1; c <= q.size; c++) {
            if (q.matrix[r][c] === 1) {
                const x = (margin + c - 1) * scale;
                const y = (margin + r - 1) * scale;
                segs.push(`M${x} ${y}h${scale}v${scale}h-${scale}z`);
            }
        }
    }
    parts.push(`<path fill="${dark}" d="${segs.join("")}"/>`);
    parts.push("</svg>");
    return parts.join("");
}

const qrcode = { encode, svg };
export { qrcode };
