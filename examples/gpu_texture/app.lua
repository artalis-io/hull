-- GPU texture processing example
--
-- Demonstrates loading images as GPU textures, processing with WGSL
-- compute shaders (color inversion), and reading back results.
--
-- Run:  hull examples/gpu_texture/app.lua
--       (requires: make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu)
--
-- Test: curl localhost:3000/health
--       curl localhost:3000/process

local gpu   = require("hull.gpu")
local image = require("hull.image")

app.manifest({
    gpu = true,
    modules = {
        "hull/gpu@1",
        "hull/image@1",
    },
})

-- Load and compile the invert shader from shaders/invert.wgsl
if gpu.available() then
    gpu.load("invert")
else
    log.warn("GPU not available — texture endpoints will return errors")
end

-- Create a 4x4 RGBA8 gradient test image programmatically
-- Each pixel: R increases left-to-right, G increases top-to-bottom, B=128, A=255
local function make_test_image()
    local pixels = {}
    for y = 0, 3 do
        for x = 0, 3 do
            local r = math.floor(x * 85)   -- 0, 85, 170, 255
            local g = math.floor(y * 85)
            local b = 128
            local a = 255
            pixels[#pixels + 1] = string.char(r, g, b, a)
        end
    end
    return image.new(4, 4, "rgba8", table.concat(pixels))
end

-- Health check
app.get("/health", function(req, res)
    res:json({
        ok = true,
        gpu = gpu.available(),
        devices = gpu.devices(),
    })
end)

-- Process: create image, upload as texture, invert colors, read back
app.get("/process", function(req, res)
    if not gpu.available() then
        res:status(503):json({ error = "GPU not available" })
        return
    end

    -- Create test image
    local img = make_test_image()

    -- Upload as persistent GPU texture
    local _, err = gpu.texture("input", img)
    if err then
        img:close()
        res:status(500):json({ error = "texture upload: " .. err })
        return
    end

    -- Create output storage texture (same dimensions)
    _, err = gpu.texture("output", nil, {
        width = img:width(),
        height = img:height(),
        format = "rgba8",
        storage = true,
    })
    if err then
        img:close()
        res:status(500):json({ error = "output texture: " .. err })
        return
    end

    -- Dispatch the invert shader
    local _result, dispatch_err = gpu.dispatch("invert", {
        textures = {
            { name = "input" },                     -- sampled (binding 0, 1)
            { name = "output", storage = true },    -- storage (binding 2)
        },
        workgroups = { x = img:width(), y = img:height(), z = 1 },
        output_texture = 2,  -- 1-indexed: read back the 2nd texture
    })

    if dispatch_err then
        img:close()
        res:status(500):json({ error = "dispatch: " .. dispatch_err })
        return
    end

    -- Read back result
    local out_img = gpu.texture_read("output")
    if not out_img then
        img:close()
        res:status(500):json({ error = "texture readback failed" })
        return
    end

    -- Compare first pixel: original (0, 0, 128, 255) -> inverted (255, 255, 127, 255)
    local out_pixels = out_img:pixels()
    local r = string.byte(out_pixels, 1)
    local g = string.byte(out_pixels, 2)
    local b = string.byte(out_pixels, 3)
    local a = string.byte(out_pixels, 4)

    res:json({
        ok = true,
        input = {
            width = img:width(),
            height = img:height(),
            format = img:format(),
            first_pixel = { r = 0, g = 0, b = 128, a = 255 },
        },
        output = {
            width = out_img:width(),
            height = out_img:height(),
            format = out_img:format(),
            first_pixel = { r = r, g = g, b = b, a = a },
        },
    })

    img:close()
    out_img:close()
end)
