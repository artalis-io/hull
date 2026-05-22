// GPU texture processing example
//
// Demonstrates loading images as GPU textures, processing with WGSL
// compute shaders (color inversion), and reading back results.
//
// Run:  hull examples/gpu_texture/app.js
//       (requires: make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu)
//
// Test: curl localhost:3000/health
//       curl localhost:3000/process

import { gpu } from "hull:gpu";
import { image } from "hull:image";

import { log } from "hull:log";
app.manifest({
    gpu: true,
    modules: [
        "hull/http-server@1",
        "hull/log@1",
        "hull/gpu@1",
        "hull/image@1",
    ],
});

// Load and compile the invert shader from shaders/invert.wgsl
if (gpu.available()) {
    gpu.load("invert");
} else {
    log.warn("GPU not available — texture endpoints will return errors");
}

// Create a 4x4 RGBA8 gradient test image programmatically
// Each pixel: R increases left-to-right, G increases top-to-bottom, B=128, A=255
function makeTestImage() {
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
    res.json({
        ok: true,
        gpu: gpu.available(),
        devices: gpu.devices(),
    });
});

// Process: create image, upload as texture, invert colors, read back
app.get("/process", (_req, res) => {
    if (!gpu.available()) {
        res.status(503).json({ error: "GPU not available" });
        return;
    }

    // Create test image
    const img = makeTestImage();

    // Upload as persistent GPU texture
    gpu.texture("input", img);

    // Create output storage texture (same dimensions)
    gpu.texture("output", null, {
        width: img.width(),
        height: img.height(),
        format: "rgba8",
        storage: true,
    });

    // Dispatch the invert shader
    gpu.dispatch("invert", {
        textures: [
            { name: "input" },                      // sampled (binding 0, 1)
            { name: "output", storage: true },       // storage (binding 2)
        ],
        workgroups: { x: img.width(), y: img.height(), z: 1 },
        outputTexture: 1,  // 0-indexed: read back the 2nd texture
    });

    // Read back result
    const outImg = gpu.textureRead("output");

    // Compare first pixel: original (0, 0, 128, 255) -> inverted (255, 255, 127, 255)
    const outPixels = new Uint8Array(outImg.pixels());

    res.json({
        ok: true,
        input: {
            width: img.width(),
            height: img.height(),
            format: img.format(),
            firstPixel: { r: 0, g: 0, b: 128, a: 255 },
        },
        output: {
            width: outImg.width(),
            height: outImg.height(),
            format: outImg.format(),
            firstPixel: {
                r: outPixels[0],
                g: outPixels[1],
                b: outPixels[2],
                a: outPixels[3],
            },
        },
    });

    img.close();
    outImg.close();
});
