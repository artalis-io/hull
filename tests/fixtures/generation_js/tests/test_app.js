// Passes only when the generation's lib/m.js shadows the file.
test("lib/m comes from the generation", () => {
    const r = test.runMain([]);
    test.eq(r.code, 0);
    test.eq(r.stdout, "entry=disk m=overlay page=disk-page\n");
});
