# Hull — Personas

Real people, real problems, real reasons to care about local-first.

---

## Daniel — Investment Banker, Vibecoder

**Who:** Managing Director at a mid-market investment bank in London. 20 years in finance. Technically literate but not a developer — he can read code, not write it. Started vibecoding with Claude Code six months ago and hasn't stopped.

**What he builds:** Deal flow tracker. Portfolio company dashboard. An internal tool that pulls financial data from Bloomberg Terminal exports, cross-references with his CRM, and generates one-page investment memos as PDFs. His analysts used to spend 4 hours on each memo. Now it takes 10 minutes.

**His problem:** Everything he builds with Claude lives on his laptop or on Vercel. The Vercel path terrifies him — client names, deal terms, and valuation models sitting on someone else's infrastructure. His compliance officer would have a stroke. The localhost path means nobody else on his team can use it.

**Why Hull:** One file. Runs on any analyst's laptop. Client data never leaves the machine. Encrypted SQLite database. Kernel sandbox that proves — provably, to a regulator — the app can't phone home. No Vercel bill. No AWS account. No compliance nightmare. He emails the file to his team, they double-click, they have the tool.

**What he'd pay:** $499 Perpetual without blinking. That's less than one hour of his billable rate. If it saves his analysts 4 hours per deal and they do 200 deals a year, the ROI is absurd.

**The quote:** "I don't want to deploy anything. I want to build a tool that works on my computer, put it on my team's computers, and never think about infrastructure again. That's it. That's the entire requirement."

---

## Priya — Product Owner at an AI Startup, Vibecoder

**Who:** Senior PO at a Series B AI company in San Francisco. Ex-Google. Manages a team of 8 engineers building an LLM-powered document processing platform. Uses Claude Code and Cursor daily — not for the company product, but for her own workflow tools.

**What she builds:** Sprint planning dashboard that pulls from Linear and formats her weekly stakeholder reports. A customer feedback classifier that reads Intercom exports and tags themes. A personal OKR tracker that generates progress narratives for board meetings. None of these are worth engineering time from her team. All of them save her 5-10 hours a week.

**Her problem:** She understands the AI-to-deployment gap better than anyone because she watches her own engineers struggle with it at scale. For her personal tools, the gap is even worse — she's not going to set up a k8s cluster for a sprint dashboard. So everything runs on `localhost:3000` and dies when she closes her terminal.

**Why Hull:** She sees Hull from two angles. Personally: her tools become files she can run anywhere, share with other POs, even sell as side projects. Professionally: she recognizes Hull as the missing distribution layer that makes AI-generated code into actual products. She's been telling her CEO that the biggest bottleneck isn't generation — it's distribution. Hull is the proof.

**What she'd pay:** $99 Standard for herself. She'd pitch the $299 Team license to her company for internal tooling. But more importantly, she'd advocate for Hull integration into their own product — "let our customers export their AI-generated workflows as Hull apps."

**The quote:** "Every AI company is solving generation. Nobody is solving distribution. The vibecoder can describe anything — but they can't ship anything. That's the gap."

---

## Gábor — Former AI CEO, Retired, Frustrated

**Who:** 58. Built and sold two companies — one in enterprise NLP (acquired 2019), one in computer vision for manufacturing (acquired 2022). Net worth is comfortable. Now "retired" but actually running a small vineyard and a property management side business with his wife in Villány, Hungary. Still follows AI closely but increasingly skeptical.

**What he wants:** Simple tools for his actual businesses. Inventory tracking for the vineyard — barrels, bottles, vintages, costs. A tenant management app for his 6 rental properties — leases, maintenance requests, payment tracking. A wine sales ledger that generates invoices compliant with Hungarian NAV requirements.

**His problem:** He's tried every AI coding tool. GitHub Copilot. Cursor. Claude Code. Lovable. Bolt. They all generate beautiful React apps that require a PhD in DevOps to deploy. He doesn't want a cloud app. He has 3 employees. He wants something that runs on the office computer in Villány, works when the internet is spotty, and doesn't cost €50/month per tool in SaaS subscriptions.

He's also deeply tired of the AI hype cycle. He built AI companies. He knows what LLMs can and can't do. He's watched his retired friends get sold $200/month "AI business tools" that are thin wrappers around GPT-4 with a database. He finds it offensive.

**Why Hull:** Hull respects his intelligence. No cloud, no subscription, no bullshit. He describes his vineyard inventory system to Claude Code, it writes Lua, `hull build` gives him a file. He puts it on the office computer. It works. His wife can use it. Backup is copying a file to the NAS. When NAV changes the invoicing API, he updates the Lua code and rebuilds. The Hungarian tax authority integration (`api.nav.gov.hu`) is declared in the app config and kernel-enforced — the app literally cannot talk to anything else.

**What he'd pay:** $499 Perpetual. Not because of the price — because of the principle. He's buying a tool, not renting access. He's done renting. He sold two companies so he'd never have to answer to anyone again. Hull's licensing model matches his worldview: pay once, own it forever.

**The quote:** "I built AI companies. I know what this technology is worth. What it's not worth is $50/month forever to store my grape harvest data on someone else's computer in Frankfurt."

---

## James — CPO at Big Tech

**Who:** Chief Product Officer at a Fortune 100 technology company. Manages a product organization of 400+ people. Stanford MBA. Previously VP of Product at two FAANG companies. Lives in the details despite the title — still reads PRDs, still prototypes, still codes on weekends.

**What he builds:** On company time: nothing with Hull — his company has its own stack. On his own time: everything. A personal CRM that tracks every person he's met at conferences, investors he's talked to, founders he's advising. A board meeting prep tool that aggregates notes from 1:1s with his directs and generates a narrative. A compensation benchmarking tool he uses during review cycles (sensitive data he absolutely cannot put in a SaaS).

**His problem:** He runs product for a company that has been burned by supply chain attacks, cloud cost overruns, and vendor lock-in at a scale most people can't imagine. He's sat in board meetings where a single SaaS vendor's pricing change cost them $40M/year. He's reviewed incident reports where a compromised npm package breached customer data. He understands, at a visceral level, what "zero dependencies" means.

For his personal tools: he can't use his company's internal platform (policy). He can't put sensitive comp data or board prep in a SaaS (obviously). He's been running Python scripts from his terminal like it's 2008.

**Why Hull:** He doesn't need Hull to solve a problem he can't solve otherwise — he's technical enough to deploy a server. He wants Hull because it's *correct*. Single binary. Six dependencies. Kernel sandbox. Reproducible builds. Ed25519 signatures. This is how he thinks software should be built. He'll use it for personal tools, recommend it to founders he advises, and eventually propose it for internal tooling pilots at his company.

**What he'd pay:** $99 Standard personally. The real value is when he walks into a board meeting at one of his three advisory companies and says "you should build on this." One recommendation from someone like James is worth more than 10,000 Hacker News upvotes.

**The quote:** "I've spent 20 years watching the industry add complexity. Layers on layers on layers. Hull is the first thing I've seen in a decade that goes the other direction. Six libraries. Two megabytes. That's not a limitation — that's a design philosophy I can get behind."

---

## Sarah — CEO at a Leading AI Company, Vibecoder

**Who:** CEO of a top-5 AI lab. PhD in machine learning. 2,000+ employees. Billions in revenue. She could have any tool built by any team. Instead, she vibecodes small utilities herself — partly because it's faster than filing a request with internal tools, partly because she genuinely enjoys it, partly because she wants to stay close to what the technology actually feels like for normal users.

**What she builds:** A personal knowledge base that ingests her reading list (papers, articles, memos) and lets her search by concept, not keyword. A meeting prep tool that pulls context from her calendar, recent Slack threads, and the attendee's last 3 interactions with her. A quick financial model builder she uses to sanity-check numbers before board meetings. A travel expense tracker that auto-categorizes receipts and generates reports for her EA.

**Her problem:** She knows better than almost anyone that AI-generated code has a last-mile problem. Her own company's tools can generate an entire application from a description — but the output still needs hosting, deployment, databases, and operational infrastructure. She sees her own employees — brilliant ML engineers — struggling to deploy simple internal tools because the DevOps overhead is disproportionate to the tool's complexity.

She also has a philosophical concern. The AI industry is creating a new dependency: the cloud. Every AI-generated app defaults to cloud deployment. The people her company is empowering to build software are simultaneously being locked into cloud infrastructure they don't control. She finds this... ironic.

**Why Hull:** Hull is the answer to a question she's been asking internally: "What does it look like when AI-generated code becomes a product the user owns, not a service the user rents?" She sees Hull as the distribution layer that completes the AI coding story. Generation is solved. Distribution is not. Hull distributes.

For her personal tools: they become files on her laptop. No dependency on her own company's infrastructure (which she can't use for personal projects anyway). No dependency on any cloud provider. Data stays local. She finds it refreshing that the one thing she uses that doesn't depend on AI infrastructure is the thing that makes AI-generated code most useful.

**What she'd pay:** Money is not the object. She'd invest. She sees Hull as a category-defining platform — the "build-to-own" layer for the AI era. If vibecoders are the new developers, Hull is their Xcode. She wants to know the founders.

**The quote:** "We solved the generation problem. We made it so anyone can describe software and have it built. But we accidentally made the cloud the default destination for everything. Hull is the exit ramp. You describe the tool, the AI writes it, and you own the result. That's the complete story. Without the exit ramp, vibecoding is just a more accessible way to become dependent on infrastructure."

---

## Marco — IT Consultant in a Small Italian Town

**Who:** 42. Solo IT consultant in Bergamo. Maintains computers, networks, and "whatever needs fixing" for 30+ local businesses — restaurants, dental offices, accounting firms, a car dealership, two gyms. Has a CompTIA cert and 15 years of hands-on experience. Not a software developer, but comfortable with configuration, scripting, and troubleshooting.

**What he wants to build:** His clients keep asking for the same things. The dentist wants an appointment scheduler that isn't Calendly (GDPR, and €15/month/seat adds up with 4 receptionists). The restaurant wants a simple reservation system. The accountant wants a client document portal. The car dealership wants an inventory tracker with photos. These are all 500-line apps that a SaaS company charges €30-50/month for.

**His problem:** He can't code from scratch. He's tried WordPress plugins, Airtable, Notion, Google Sheets with scripts. Each solution creates a new dependency, a new monthly bill for his client, and a new thing that breaks when the vendor changes their pricing or API. His clients are small businesses — they don't have IT budgets, they have Marco.

**Why Hull:** Marco describes the appointment scheduler to Claude Code. It writes Lua. `hull build` gives him a file. He installs it on the dentist's reception computer. It runs. The data is a SQLite file he can back up to a USB stick during his monthly visit. No internet required for daily operation. NAV-compliant invoicing built in (for the clients that need it). He charges the dentist €500 one-time for the tool and €200/year for maintenance. The dentist was paying €720/year for Calendly. Everyone wins.

He does this for 10 clients and suddenly he has a small software business on top of his consulting business. He didn't learn to code. He learned to describe.

**What he'd pay:** $99 Standard license. The ROI is immediate — his first Hull project for a client pays for the license 5x over. By the end of the year, he's making more from Hull apps than from fixing printers.

**The quote:** "My clients don't need SaaS. They need a tool on their computer that does one thing. I used to tell them that doesn't exist anymore. Now it does."

---

## Lisa — Compliance Officer at a Defense Contractor

**Who:** Director of Software Compliance at a mid-tier US defense contractor. Responsible for certifying all software tools used on classified networks. Former Air Force, now civilian side. Deals with NIST 800-171, CMMC Level 2, and the daily nightmare of getting any software approved for use on air-gapped systems.

**What she needs:** Her engineers keep building internal tools — test data generators, report formatters, configuration managers — using Python scripts with pip dependencies. Every tool that touches a classified network must go through a months-long approval process. Each dependency in the supply chain must be documented, reviewed, and approved. A Python script with 47 pip packages is 47 line items on a software bill of materials, each requiring individual review. Her team spends more time certifying tools than building them.

**Her problem:** The supply chain review process was designed for a world where software had 5-10 dependencies. Modern software stacks have 500-1,500. The process hasn't scaled. Her engineers are productive with AI coding assistants but can't deploy what they build to the networks where it's needed, because the certification overhead is prohibitive. The irony: the tools are simple, the certification is complex.

**Why Hull:** Six dependencies. Total. She can enumerate the entire supply chain in one paragraph: Keel, Lua, SQLite, mbedTLS, TweetNaCl, pledge/unveil. All vendored, all auditable, all with established security track records. One SBOM entry instead of hundreds. The binary is self-contained — no runtime installation, no package manager on the classified network, no internet access required (by design). Kernel sandbox provides provable containment. Ed25519 signatures provide provable integrity. `hull verify` gives her a one-command attestation she can attach to the certification package.

Development can be fully air-gapped: engineers use a local LLM on isolated hardware. Source code never leaves the secure facility. The entire pipeline — from code generation to build to deployment — operates within the air gap.

**What she'd pay:** Enterprise tier. She'd sign a $10K/year contract for compliance documentation, priority support, and a direct line to the Hull team for security questions. The real value is the hundreds of engineering hours saved on certification per year.

**The quote:** "You're telling me the entire supply chain is six C libraries, the binary is self-contained, it runs air-gapped, and the sandbox is kernel-enforced? Give me the paperwork. This is the easiest certification I've done in ten years."

---

## Tom — Indie Hacker, Solo Developer

**Who:** 29. Full-stack developer. Quit his job at a startup 18 months ago to build his own products. Lives in Lisbon on a modest runway. Has shipped three products — a habit tracker, a bookmark manager, and a micro-SaaS for freelancer invoicing. All on Vercel + Supabase.

**What he builds:** Local-first tools for niche audiences. He's spotted a pattern: a lot of the software people pay $20/month for should be a one-time purchase that runs offline. Password managers, personal finance trackers, recipe organizers, journal apps. He wants to build a portfolio of small, polished, one-time-purchase desktop tools.

**His problem:** His current stack (React + Node + Postgres + Vercel) has an inherent floor cost per product. Each app needs hosting ($5-20/month), a database ($10-25/month), and monitoring. Three products = ~$100/month in infrastructure before a single customer pays. If a product doesn't take off, the infrastructure cost keeps ticking. He's paying $1,200/year to keep his three products alive.

He also hates the recurring revenue pressure. He charges $8/month because that's what the market expects. Customers churn. He spends time on retention marketing instead of building. He wants to sell a product once and move on to the next one.

**Why Hull:** Zero infrastructure cost. Every product is a file. No server, no database server, no monitoring, no hosting bill. His portfolio of 10 products costs $0/month to run after they're built. He sells each for $29-49 one-time. No churn, no retention problem. Customers get a file, he gets the money, both parties are done.

Ed25519 licensing means he can have a Standard and Pro tier if he wants. AGPL means his free-tier tools generate awareness (source is visible). Commercial license means his paid tools protect his code.

**What he'd pay:** $99 Standard. He'll make it back on his first sale. Within a year, his Hull portfolio generates more revenue than his three SaaS products combined — with zero ongoing costs.

**The quote:** "SaaS is a treadmill. You never stop running. Hull lets me build something, sell it, and move on. The product works forever. The customer is happy forever. I'm free to build the next thing."
