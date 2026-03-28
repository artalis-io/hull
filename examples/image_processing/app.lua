-- Image processing example
--
-- Demonstrates the image module: create images from raw pixels,
-- encode to PNG, decode from encoded bytes, and inspect properties.
--
-- Run:  hull examples/image_processing/app.lua
--
-- Test: curl localhost:3000/health
--       curl localhost:3000/create
--       curl localhost:3000/info

app.manifest({})

-- Create a 4x4 RGBA8 gradient test image programmatically
local function make_gradient()
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
    res:json({ status = "ok" })
end)

-- Create a test image, encode to PNG, return hex-encoded bytes and metadata
app.get("/create", function(req, res)
    local img = make_gradient()

    -- Encode to PNG
    local png_data, err = image.encode(img, "png")
    if not png_data then
        img:close()
        res:status(500):json({ error = err })
        return
    end

    -- Encode PNG bytes as hex for JSON transport
    local hex = {}
    for i = 1, #png_data do
        hex[#hex + 1] = string.format("%02x", string.byte(png_data, i))
    end

    res:json({
        ok = true,
        width = img:width(),
        height = img:height(),
        format = img:format(),
        pixel_size = img:size(),
        png_size = #png_data,
        png_hex = table.concat(hex),
    })

    img:close()
end)

-- Create image, encode to PNG, decode back, verify round-trip
app.get("/info", function(req, res)
    local img = make_gradient()

    -- Encode to PNG
    local png_data, encode_err = image.encode(img, "png")
    if not png_data then
        img:close()
        res:status(500):json({ error = encode_err })
        return
    end

    -- Decode back from PNG
    local decoded, decode_err = image.decode(png_data)
    if not decoded then
        img:close()
        res:status(500):json({ error = decode_err })
        return
    end

    -- Verify round-trip: dimensions and pixel data should match
    local original_pixels = img:pixels()
    local decoded_pixels = decoded:pixels()
    local match = (original_pixels == decoded_pixels)

    res:json({
        ok = true,
        original = {
            width = img:width(),
            height = img:height(),
            format = img:format(),
            size = img:size(),
        },
        decoded = {
            width = decoded:width(),
            height = decoded:height(),
            format = decoded:format(),
            size = decoded:size(),
        },
        round_trip_match = match,
    })

    img:close()
    decoded:close()
end)
