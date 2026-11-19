--
-- hull.app_sig - The app layer's signed payload.
--
-- `hull build --sign` signs these bytes, and `hull verify` and `hull inspect`
-- rebuild them from a package.sig to check the signature, so all three go
-- through this one function: a field signed by one and left out by another
-- read every such signature as INVALID (`hull inspect` did that to every app
-- built through the pipeline, whose `generated` it dropped).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local json = require("hull.json")

local M = {}

-- The JSON `hull build` signs for a package.sig's app layer. A field the
-- signature does not carry is left out (json.encode drops nil), as the
-- builds that predate it signed without it. A legacy hull.sig (no
-- binary_hash) signed only its files and manifest.
function M.payload(sig)
    if sig.binary_hash then
        return json.encode({
            binary_hash      = sig.binary_hash,
            build            = sig.build,
            files            = sig.files,
            generated        = sig.generated,
            manifest         = sig.manifest,
            modules_resolved = sig.modules_resolved,
            platform         = sig.platform,
            trampoline_hash  = sig.trampoline_hash,
        })
    end
    return json.encode({ files = sig.files, manifest = sig.manifest })
end

return M
