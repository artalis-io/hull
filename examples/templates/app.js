/**
 * Template engine example — JavaScript
 *
 * Demonstrates inheritance, includes, filters, loops, and CSP nonce.
 *
 * Run:  hull dev examples/templates/app.js
 *       hull examples/templates/app.js
 */

import { template } from "hull:template";

// Sample data
const users = [
    { name: "Alice",   email: "alice@example.com" },
    { name: "Bob",     email: "bob@example.com" },
    { name: "Charlie", email: null },
];

const features = [
    "  Template inheritance  ",
    "  Include partials  ",
    "  Built-in filters  ",
    "  HTML auto-escaping  ",
    "  Compiled & cached  ",
];

app.get("/", (req, res) => {
    const html = template.render("pages/home.html", {
        site_name:    "Hull Demo",
        year:         new Date().getFullYear().toString(),
        users:        users,
        features:     features,
        html_snippet: '<em>bold & "quoted"</em>',
    });
    res.html(html);
});

app.get("/about", (req, res) => {
    const html = template.render("pages/about.html", {
        site_name: "Hull Demo",
        year:      new Date().getFullYear().toString(),
        version:   "0.1.0",
    });
    res.html(html);
});

app.get("/users", (req, res) => {
    res.json(users);
});
