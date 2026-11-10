import { app } from "hull:app";
import { jobs } from "hull:jobs";
// The JS twin of jobs.lua (audit 44).
app.manifest({
    name: "state-a44-jobs",
    state: { name: "hull-e2e-a44j" },
    modules: ["hull/db@1", "hull/jobs@1"],
    databases: { named: { default: "@state/data.db" } },
});
jobs.init();
jobs.enqueue("top", { n: 1 });
app.main(async () => {
    jobs.enqueue("main", { n: 2 });
    console.log("enqueued=2");
    return 0;
});
