// Image processing example
//
// Demonstrates the image module: create images from raw pixels,
// encode to PNG, decode from encoded bytes, and inspect properties.
//
// Run:  hull examples/image_processing/app.js
//
// Test: curl localhost:3000/health
//       curl localhost:3000/create
//       curl localhost:3000/info

import { image } from "hull:image";

app.manifest({});

// Create a 4x4 RGBA8 gradient test image programmatically
function makeGradient() {
    const buf = new ArrayBuffer(4 * 4 * 4);
    const view = new Uint8Array(buf);
    let offset = 0;
    for (let y = 0; y < 4; y++) {
        for (let x = 0; x < 4; x++) {
            view[offset++] = Math.floor(x * 85);   // R: 0, 85, 170, 255
            view[offset++] = Math.floor(y * 85);   // G
            view[offset++] = 128;                   // B
            view[offset++] = 255;                   // A
        }
    }
    return image.new(4, 4, "rgba8", buf);
}

// Health check
app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// Create a test image, encode to PNG, return hex-encoded bytes and metadata
app.get("/create", (_req, res) => {
    const img = makeGradient();

    // Encode to PNG
    const pngData = image.encode(img, "png");

    // Encode PNG bytes as hex for JSON transport
    const pngView = new Uint8Array(pngData);
    const hexChars = [];
    for (let i = 0; i < pngView.length; i++) {
        hexChars.push(pngView[i].toString(16).padStart(2, "0"));
    }

    res.json({
        ok: true,
        width: img.width(),
        height: img.height(),
        format: img.format(),
        pixelSize: img.size(),
        pngSize: pngView.length,
        pngHex: hexChars.join(""),
    });

    img.close();
});

// Create image, encode to PNG, decode back, verify round-trip
app.get("/info", (_req, res) => {
    const img = makeGradient();

    // Encode to PNG
    const pngData = image.encode(img, "png");

    // Decode back from PNG
    const decoded = image.decode(pngData);

    // Verify round-trip: dimensions should match
    const origPixels = new Uint8Array(img.pixels());
    const decPixels = new Uint8Array(decoded.pixels());
    let match = origPixels.length === decPixels.length;
    if (match) {
        for (let i = 0; i < origPixels.length; i++) {
            if (origPixels[i] !== decPixels[i]) {
                match = false;
                break;
            }
        }
    }

    res.json({
        ok: true,
        original: {
            width: img.width(),
            height: img.height(),
            format: img.format(),
            size: img.size(),
        },
        decoded: {
            width: decoded.width(),
            height: decoded.height(),
            format: decoded.format(),
            size: decoded.size(),
        },
        roundTripMatch: match,
    });

    img.close();
    decoded.close();
});
