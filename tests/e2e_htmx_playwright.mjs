/*
 * e2e_htmx_playwright.mjs — actual browser-side checks for the
 * HTMX example apps. Invoked by tests/e2e_htmx_playwright.sh
 * after it has spun up the relevant hull dev server.
 *
 * Usage: node e2e_htmx_playwright.mjs <suite> <base-url>
 *   suite ∈ { widgets, photos }
 *
 * Exit code: 0 if all checks pass, 1 otherwise.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// Playwright lives outside this tree (tests/.playwright/node_modules/).
// ESM ignores NODE_PATH, so resolve it via a dynamic import from an
// absolute path supplied by the orchestrator script.
import { mkdir }   from "node:fs/promises";
import { join }   from "node:path";

const pwDir = process.env.PLAYWRIGHT_DIR;
if (!pwDir) {
    console.error("e2e_htmx_playwright.mjs: PLAYWRIGHT_DIR env var must be set "
        + "to the directory containing node_modules/playwright (the orchestrator "
        + "script sets this)");
    process.exit(2);
}
const { chromium }   = await import(pwDir + "/node_modules/playwright/index.mjs");
const { AxeBuilder } = await import(pwDir + "/node_modules/@axe-core/playwright/dist/index.mjs");

// Where to drop failure artifacts (trace + final screenshot). Set
// by the orchestrator; when unset we skip artifact capture, which
// is fine for ad-hoc local invocations of the .mjs.
const artifactRoot = process.env.HULL_PW_ARTIFACTS || null;

const [suite, base, runtime] = process.argv.slice(2);
if (!suite || !base) {
    console.error("usage: node e2e_htmx_playwright.mjs <widgets|photos> <base-url> [runtime]");
    process.exit(2);
}
// Runtime label is purely cosmetic for now — used in the banner +
// (when traces ship) the artifact filename. Default "lua" matches
// the historical default and keeps single-arg invocations working.
const rt = runtime || "lua";
const suiteLabel = `${suite}[${rt}]`;

let pass = 0, fail = 0;
const logs = [];

function ok(msg)        { console.log("  PASS:", msg); pass++; }
function ko(msg, extra) {
    console.log("  FAIL:", msg + (extra ? ` — ${extra}` : ""));
    fail++;
}

// Sleep on a real timer. `page.waitForTimeout` is fine in Playwright
// but `setTimeout` is friendlier in mixed contexts.
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function attachDiagnostics(page) {
    page.on("console", (m) => {
        if (m.type() !== "warning" && m.type() !== "error") return;
        const text = m.text();
        // Strict CSP preset blocks Function() eval, which trips
        // htmx's optional `[expr]` trigger-filter syntax. The widget
        // tier uses `keyup[key=='Enter']` for keyboard accessibility,
        // so each interaction logs htmx:evalDisallowedError. Expected
        // under csp = "htmx"; not actionable from the test side.
        if (text === "htmx:evalDisallowedError") return;
        logs.push(`[console.${m.type()}] ${text}`);
    });
    page.on("pageerror", (e) => logs.push(`[pageerror] ${e.message}`));
    page.on("requestfailed", (r) => {
        const url = r.url();
        // Favicons can fail on some headless setups; not actionable.
        if (url.startsWith("data:")) return;
        logs.push(`[reqfailed] ${url} — ${r.failure()?.errorText || "?"}`);
    });
}

// Run @axe-core against the current page and treat the WCAG verdict
// as a test assertion. We fail HARD on `critical` or `serious`
// findings (broken semantics, missing labels, keyboard traps) and
// log `moderate` / `minor` as informational — those are real
// concerns but more theme-dependent (e.g., Pico's color-contrast
// choices) and shouldn't gate every CI run while a theme is in
// flux. The full violation list always prints so debugging
// doesn't need a second run with --debug.
async function expectA11y(page, label) {
    const results = await new AxeBuilder({ page }).analyze();
    const v = results.violations;
    const critSer = v.filter((x) => x.impact === "critical" || x.impact === "serious");
    const lesser  = v.filter((x) => x.impact === "moderate" || x.impact === "minor");
    if (critSer.length === 0) {
        ok(`a11y: ${label} clean (${lesser.length} moderate/minor noted)`);
    } else {
        const summary = critSer.map((x) =>
            `${x.impact}/${x.id} (${x.nodes.length} node${x.nodes.length === 1 ? "" : "s"})`
        ).join(", ");
        ko(`a11y: ${label} has ${critSer.length} critical/serious violation(s): ${summary}`);
    }
    // Always emit the full violation table to logs so failures show
    // node selectors + help URLs without a second debugger pass.
    for (const x of v) {
        logs.push(`[axe.${x.impact}] ${x.id} — ${x.description} (${x.helpUrl})`);
        for (const n of x.nodes.slice(0, 3)) {
            logs.push(`  • ${n.target.join(" ")}`);
        }
        if (x.nodes.length > 3) logs.push(`  • …and ${x.nodes.length - 3} more`);
    }
}

async function expectCssApplied(page) {
    // The regression we want to catch: "stylesheets serve OK but
    // declarations don't apply" (e.g., var(--missing) with no
    // fallback collapses every rule, like the Pico-zinc-typo bug
    // that motivated this test).
    //
    // Two independent signals:
    //   A) every <link rel=stylesheet> shows up in document.styleSheets
    //      AND has actually-parsed cssRules (a 404 or parse failure
    //      drops the entry; an empty rule list means "loaded but
    //      empty" which is also broken).
    //   B) <h1> has had its margin and font-size changed by Pico
    //      relative to the UA stylesheet defaults. Default UA h1 in
    //      Chromium is `2em` (= 32 px @ 16 px base) with a top
    //      margin near 21 px. Pico bumps the font-size up and resets
    //      margin. If h1 still has UA defaults, CSS isn't applying.
    const linkCount = await page.locator("link[rel=stylesheet]").count();
    const sheetInfo = await page.evaluate(() =>
        Array.from(document.styleSheets)
            .filter((s) => s.href !== null)
            .map((s) => {
                try { return { href: s.href, rules: s.cssRules.length }; }
                catch { return { href: s.href, rules: -1 }; }  // CORS-blocked
            }));
    if (sheetInfo.length === linkCount && sheetInfo.every((s) => s.rules > 0)) {
        ok(`all ${linkCount} stylesheet(s) loaded with rules`);
    } else {
        const broken = sheetInfo
            .filter((s) => s.rules <= 0)
            .map((s) => `${s.href} (rules=${s.rules})`)
            .join(", ");
        ko(`stylesheet(s) loaded with no rules — ${broken || "(none — count mismatch)"}`);
    }

    // Pico applies font-family and changes h1 sizing. Both are
    // observable through computed style. We pick font-family on
    // <body> because it's the most reliable: every Pico variant
    // sets it; UA default is a serif (Times New Roman on macOS,
    // Liberation Serif on Linux). Anything sans-serif means Pico
    // (or a sibling CSS) is active.
    const font = await page.evaluate(() =>
        window.getComputedStyle(document.body).fontFamily);
    if (font && /sans-serif|system-ui|-apple-system|Helvetica|Arial|Inter/i.test(font)) {
        ok(`body font-family is sans-serif ("${font.split(",")[0].trim()}")`);
    } else {
        ko(`body font-family is "${font}" — looks like Pico didn't apply`);
    }
}

// ─── htmx_widgets_register ────────────────────────────────────────
//
// Test ordering note: the sort widget currently uses hx-swap="outerHTML"
// on the <th>, but its response is the WHOLE grid (table + pagination +
// action bar). When the click fires, the <th> is replaced by the grid
// chunk, which corrupts the DOM for any subsequent widget that targeted
// the original layout. So sort-click is tested LAST, after every other
// widget has had its turn against a clean DOM. (Filed as a sort-widget
// bug to address separately — the hx-swap should be the parent grid,
// not the header itself.)
async function runWidgets(page) {
    await page.goto(base, { waitUntil: "networkidle" });

    await expectCssApplied(page);
    await expectA11y(page, "initial home page");

    const h1 = await page.locator("h1").innerText();
    if (/Asset register/i.test(h1)) ok("page heading renders");
    else ko(`heading unexpected: "${h1}"`);

    const initialRows = await page.locator("table.hull-table tbody tr").count();
    if (initialRows === 8) ok("initial table has 8 rows");
    else ko(`expected 8 rows initially, got ${initialRows}`);

    // ── search widget (debounced, no eval-filter trigger) ────────
    // Two playwright gotchas baked into this sequence:
    //   1. waitForResponse must be set up BEFORE the action that
    //      triggers the request, or you race against the response.
    //   2. htmx's `input changed delay:250ms` listens for real
    //      `input` events per keystroke — locator.fill() can dispatch
    //      a single synthetic event htmx misses; keyboard.type() after
    //      focus() reliably fires the chain.
    {
        const respP = page.waitForResponse(
            (r) => r.url().includes("/search?q=") && r.status() === 200,
            { timeout: 4000 }).catch(() => null);
        await page.locator("#search-input").focus();
        await page.keyboard.type("Pump", { delay: 30 });
        const resp = await respP;
        if (!resp) {
            ko("search widget: server never replied within 4 s of input");
        } else {
            await sleep(120);
            const filteredRows = await page.locator("table.hull-table tbody tr").count();
            if (filteredRows >= 1 && filteredRows < initialRows) {
                ok(`search widget: filtered to ${filteredRows} row(s)`);
            } else {
                ko(`search widget: expected 1..${initialRows - 1} rows, got ${filteredRows}`);
            }
        }
        // Reset the filter so the next tests see baseline rows.
        const clearP = page.waitForResponse(
            (r) => r.url().includes("/search?q=") && r.status() === 200,
            { timeout: 2000 }).catch(() => null);
        await page.locator("#search-input").focus();
        await page.keyboard.press("Control+A");
        await page.keyboard.press("Backspace");
        await clearP;
        await sleep(120);
    }

    // ── inline-edit widget — plain `click` works under strict CSP
    {
        const firstEditable = page.locator(".hull-inline-edit-view").first();
        await firstEditable.click();
        const editorAppeared = await page.locator("form.hull-inline-edit-form input")
            .first().waitFor({ timeout: 2000 }).then(() => true).catch(() => false);
        if (editorAppeared) {
            ok("inline-edit widget: clicking shows editor");
            await page.keyboard.press("Escape");
            await sleep(150);
        } else {
            ko("inline-edit widget: editor never appeared on click");
        }
    }

    // ── form widget — server-side validation error renders inline
    // The form fields have HTML5 `required` attribute, so a normal
    // empty submit gets blocked by the browser before htmx fires.
    // To test server-side validation, drop the `required` attrs
    // temporarily so the request actually reaches the server with
    // empty values, then assert the server returned the form
    // fragment with .hull-form-error markup.
    {
        await page.evaluate(() => {
            document.querySelectorAll("#new-form [required]")
                .forEach((el) => el.removeAttribute("required"));
        });
        const respP = page.waitForResponse(
            (r) => r.url().endsWith("/assets") && r.status() === 200,
            { timeout: 3000 }).catch(() => null);
        await page.locator("#f-name").fill("");
        await page.locator("#f-category").fill("");
        await page.locator('#new-form button[type="submit"]').click();
        await respP;
        const errsAppeared = await page.locator(".hull-form-error")
            .first().waitFor({ timeout: 2000 }).then(() => true).catch(() => false);
        if (errsAppeared) {
            const errCount = await page.locator(".hull-form-error").count();
            ok(`form widget: ${errCount} server-side error(s) rendered inline`);
        } else {
            ko("form widget: empty submit produced no inline error");
        }
    }

    // ── form widget + toast widget — valid submit ────────────────
    const tagName = `e2e-${Date.now()}`;
    {
        const respP = page.waitForResponse(
            (r) => r.url().endsWith("/assets") && r.status() === 200,
            { timeout: 3000 }).catch(() => null);
        await page.locator("#f-name").fill(tagName);
        await page.locator("#f-category").fill("test");
        await page.locator('#new-form button[type="submit"]').click();
        await respP;
        const toastAppeared = await page.locator("#hull-toast .hull-toast-item")
            .first().waitFor({ timeout: 3000 }).then(() => true).catch(() => false);
        if (toastAppeared) {
            const toastText = (await page.locator("#hull-toast .hull-toast-item")
                .first().innerText()).trim();
            if (toastText.includes(tagName)) {
                ok(`toast widget: appeared with row name ("${toastText}")`);
            } else {
                ko(`toast widget: text mismatch, got "${toastText}"`);
            }
        } else {
            ko("toast widget: no toast after successful submit");
        }
    }

    // ── pagination widget ────────────────────────────────────────
    const pageLinks = await page.locator(".hull-pagination li a").count();
    if (pageLinks >= 2) {
        ok(`pagination widget: ${pageLinks} page link(s) rendered`);
    } else {
        ko(`pagination widget: expected >=2 links, got ${pageLinks}`);
    }

    // ── confirm widget — delete opens <dialog> ───────────────────
    {
        const delBtn = page.locator(".del-btn").first();
        if ((await delBtn.count()) === 0) {
            ko("confirm widget: no delete buttons visible to click");
        } else {
            await delBtn.click();
            const opened = await page.locator("dialog#hull-confirm[open]")
                .waitFor({ timeout: 2000 }).then(() => true).catch(() => false);
            if (opened) {
                ok("confirm widget: clicking delete opens dialog");
                await page.locator("dialog#hull-confirm .hull-confirm-no").click();
                await sleep(100);
            } else {
                ko("confirm widget: dialog never opened");
            }
        }
    }

    // ── sort widget — rendered attrs + activation paths ──────────
    // Last because earlier the widget shipped hx-swap="outerHTML"
    // on the <th>, which replaced the whole header with the grid
    // response and corrupted later locators. The swap default is
    // now innerHTML (the example app passes swap="innerHTML"
    // explicitly), so DOM stays sane — but the ordering convention
    // is preserved in case a future change reintroduces the issue.
    {
        const sortHeader = page.locator('th[data-sort-column="name"]').first();
        const hxGet = await sortHeader.getAttribute("hx-get");
        if (hxGet && hxGet.startsWith("/search?sort=name")) {
            ok(`sort widget: header rendered with hx-get=${hxGet}`);
        } else {
            ko(`sort widget: hx-get attribute wrong: "${hxGet}"`);
        }
        const respP = page.waitForResponse(
            (r) => r.url().includes("/search?sort=") && r.status() === 200,
            { timeout: 3000 }).catch(() => null);
        await sortHeader.click();
        const resp = await respP;
        if (resp) ok(`sort widget: click triggered ${resp.url().split("?")[1]}`);
        else ko("sort widget: click did not trigger /search?sort=…");
    }

    // The click above swaps #grid contents (innerHTML); the th
    // elements get rebuilt. Give htmx a tick to finish the swap so
    // the keyboard tests below focus the NEW elements, not the
    // stale ones in the dying DOM.
    await sleep(200);

    // ── sort widget — keyboard activation (regression for the
    // strict-CSP fix). The widget puts role="button" tabindex="0"
    // on a <th>, which doesn't natively dispatch click on
    // Enter/Space — that's sort.js's job. Two checks: Enter and
    // Space both fire the underlying click + hx-get.
    {
        // Enter on "category" header.
        const enterHeader = page.locator('th[data-sort-column="category"]').first();
        const respEnter = page.waitForResponse(
            (r) => r.url().includes("/search?sort=category") && r.status() === 200,
            { timeout: 3000 }).catch(() => null);
        await enterHeader.focus();
        await page.keyboard.press("Enter");
        if (await respEnter) ok("sort widget: Enter key fires hx-get (keyboard a11y)");
        else ko("sort widget: Enter key did not fire request");
    }

    // Same swap-settle reason as above — the Enter response
    // replaces #grid contents and the status header gets rebuilt.
    await sleep(200);

    {
        // Space on "status" header. Also implicitly verifies that
        // sort.js preventDefault()s the Space keydown — without it,
        // the browser would scroll the page instead of activating
        // the header. (Playwright doesn't fail on scroll, so we
        // catch the failure mode via the missing request.)
        const spaceHeader = page.locator('th[data-sort-column="status"]').first();
        const respSpace = page.waitForResponse(
            (r) => r.url().includes("/search?sort=status") && r.status() === 200,
            { timeout: 3000 }).catch(() => null);
        await spaceHeader.focus();
        await page.keyboard.press("Space");
        if (await respSpace) ok("sort widget: Space key fires hx-get (keyboard a11y)");
        else ko("sort widget: Space key did not fire request");
    }
}

// ─── hypermedia_photos ────────────────────────────────────────────
async function runPhotos(page) {
    await page.goto(base, { waitUntil: "networkidle" });

    await expectCssApplied(page);
    await expectA11y(page, "initial home page");

    // The app calls the entry-list region #entry-feed; its presence
    // proves SSR + CSRF + session middleware all ran successfully
    // (any of those failing would crash the response).
    const feed = await page.locator("#entry-feed").count();
    if (feed === 1) ok("entry feed region renders");
    else ko(`expected #entry-feed, found ${feed}`);

    // CSRF + session middleware should have set up scaffolding —
    // verify a session cookie was issued on first request.
    const cookies = await page.context().cookies();
    const sessionCookie = cookies.find((c) => /session|csrf/i.test(c.name));
    if (sessionCookie) {
        ok(`session/csrf cookie set ("${sessionCookie.name}")`);
    } else {
        ko(`no session/csrf cookie set (got ${cookies.length} cookies)`);
    }

    // CSP middleware emits a per-request nonce — verify the header
    // is present AND contains a nonce-* token. Regression for
    // "dropped CSP middleware in a refactor".
    const csp = (await page.request.get(base)).headers()["content-security-policy"];
    if (csp && /nonce-/.test(csp)) {
        ok("CSP nonce middleware emits Content-Security-Policy header");
    } else {
        ko(`CSP header missing or has no nonce: "${csp || "(none)"}"`);
    }

    // Search input rendered via the htmx widget tier. Both Lua AND
    // JS variants of the example app pre-render the attrs via
    // hull/web/htmx/search; a missing hx-get here is a parity
    // regression in one variant. (The widgets suite already
    // exercises debounce + filter end-to-end; this assertion is
    // about the SSR shape, not the runtime behavior.)
    const searchInputCount = await page.locator("#search-input").count();
    if (searchInputCount !== 1) {
        ko(`search input not rendered (found ${searchInputCount})`);
    } else {
        const hxGet = await page.locator("#search-input").getAttribute("hx-get");
        if (hxGet) ok(`search widget renders (hx-get=${hxGet})`);
        else ko("search widget rendered without hx-get attribute (parity regression?)");
    }

    // ── CRUD round-trip: create + delete-via-confirm ─────────────
    // Real exercise: create an entry via the form, then delete it
    // via the confirm-dialog yes path. The CSRF token + session
    // cookie travel automatically with form submits; the delete
    // flow goes through hx-delete on a standalone button, which
    // exercises the confirm widget's htmx.ajax resume path (the
    // workaround for htmx 2.0.9's issueRequest no-op on non-form
    // verbs). A regression in either CSRF / session / template /
    // confirm-widget would surface here.
    const entryTitle = `e2e-${rt}-${Date.now()}`;
    let entryRendered = false;
    {
        const respP = page.waitForResponse(
            (r) => r.url().endsWith("/entries") && r.request().method() === "POST"
                && r.status() === 200,
            { timeout: 4000 }).catch(() => null);
        await page.locator('#new-entry input[name="title"]').fill(entryTitle);
        await page.locator('#new-entry button[type="submit"]').click();
        const resp = await respP;
        if (!resp) {
            ko(`CRUD create: POST /entries never replied (CSRF/session issue?)`);
        } else {
            await sleep(150);
            entryRendered = await page.locator(`#entry-feed :text("${entryTitle}")`)
                .first().waitFor({ timeout: 2000 })
                .then(() => true).catch(() => false);
            if (entryRendered) ok(`CRUD create: new entry "${entryTitle}" rendered in feed`);
            else ko(`CRUD create: entry submitted but title not visible in feed`);
        }
    }

    if (entryRendered) {
        const delBtn = page.locator(`#entry-feed`)
            .locator(`text="${entryTitle}"`).first()
            .locator('xpath=ancestor::*[@id and starts-with(@id, "entry-")]')
            .locator('button[hx-delete]').first();
        if ((await delBtn.count()) === 0) {
            ko("CRUD delete: no delete button found on the new entry row");
        } else {
            await delBtn.click();
            const dialogOpened = await page.locator("dialog#hull-confirm[open]")
                .waitFor({ timeout: 2000 }).then(() => true).catch(() => false);
            if (!dialogOpened) {
                ko("CRUD delete: confirm dialog never opened on delete click");
            } else {
                const respP = page.waitForResponse(
                    (r) => r.url().match(/\/entries\/\d+$/) && r.request().method() === "DELETE"
                        && r.status() === 200,
                    { timeout: 4000 }).catch(() => null);
                await page.locator("dialog#hull-confirm .hull-confirm-yes").click();
                const resp = await respP;
                if (!resp) {
                    ko("CRUD delete: DELETE /entries/:id never fired after confirm yes");
                } else {
                    await sleep(200);
                    const gone = (await page.locator(
                        `#entry-feed :text("${entryTitle}")`).count()) === 0;
                    if (gone) ok(`CRUD delete: "${entryTitle}" removed from feed after confirm`);
                    else ko(`CRUD delete: response succeeded but title still visible`);
                }
            }
        }
    }
}

// ─── main ─────────────────────────────────────────────────────────
const browser = await chromium.launch({ headless: true });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true });
const page = await ctx.newPage();
attachDiagnostics(page);

// Tracing captures every action, network request, DOM snapshot, and
// console event. The trace is only KEPT when the suite fails — on
// success we stop without writing, since the artifacts add up fast
// (each suite trace is ~1-3 MB). `snapshots: true` records DOM at
// each action so the trace viewer can rewind; `screenshots: true`
// adds per-action thumbnails; `sources: true` embeds the test
// source so failures show the line that ran. View with:
//     npx playwright show-trace path/to/trace.zip
const suiteArtifactDir = artifactRoot ? join(artifactRoot, `${suite}-${rt}`) : null;
if (artifactRoot) {
    await ctx.tracing.start({
        screenshots: true,
        snapshots:   true,
        sources:     true,
        title:       suiteLabel,
    });
}

console.log(`── e2e_htmx_playwright[${suiteLabel}] → ${base} ──`);

try {
    if (suite === "widgets") {
        await runWidgets(page);
    } else if (suite === "photos") {
        await runPhotos(page);
    } else {
        ko(`unknown suite: ${suite}`);
    }
} catch (e) {
    ko(`uncaught exception in ${suiteLabel}`, e.message);
} finally {
    if (artifactRoot) {
        if (fail > 0) {
            // Failure: persist both the trace and a fresh full-page
            // screenshot of the final state. The trace is the rich
            // artifact; the screenshot is what someone clicks first
            // in CI's artifact tab. Always emit both so debuggers
            // don't have to install playwright to see "what did
            // the page look like when it died".
            await mkdir(suiteArtifactDir, { recursive: true });
            await page.screenshot({
                path: join(suiteArtifactDir, "final.png"),
                fullPage: true,
            }).catch((e) => console.log("  (screenshot failed:", e.message + ")"));
            await ctx.tracing.stop({ path: join(suiteArtifactDir, "trace.zip") });
        } else {
            // Success: discard the buffered trace data without
            // writing to disk. Keeps disk + artifact upload small.
            await ctx.tracing.stop();
        }
    }
    await browser.close();
}

console.log("");
console.log(`${suiteLabel}: ${pass} passed, ${fail} failed`);
if (fail > 0 && logs.length) {
    console.log("── browser diagnostics ──");
    for (const line of logs) console.log(line);
}
if (fail > 0 && suiteArtifactDir) {
    console.log(`── artifacts: ${suiteArtifactDir} ──`);
}
process.exit(fail > 0 ? 1 : 0);
