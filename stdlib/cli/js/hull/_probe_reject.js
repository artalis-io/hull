// hull:_probe_reject - a tooling module that REJECTS on load via top-level await, to
// exercise the entry-rejection branch and prove session reuse stays clean afterward.
// Throwaway Slice-1 test scaffolding (loaded only when a test names it as the entry);
// Slice 2 removes it with the rest of the probe bundle.
// SPDX-License-Identifier: AGPL-3.0-or-later
await Promise.reject(new Error("probe reject on load"));
// Never reached (the module evaluation promise rejects above).
globalThis.__hull_frontend = { analyze: () => ({ status: "ok" }) };
