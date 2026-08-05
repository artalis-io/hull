/**
 * @file commands/jobs.h
 * @brief `hull jobs` — durable background job queue operations.
 *
 * Today the only subcommand is `worker`, which runs an app as a dedicated
 * job-worker process (its `app.main` drives `jobs.run_worker`). Ops
 * subcommands (dead / retry / cleanup) land with jobs Phase 5.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMMANDS_JOBS_H
#define HL_COMMANDS_JOBS_H

#include "hull/commands/dispatch.h"

/** @brief Entry point — invoked by the command dispatcher. */
int hl_cmd_jobs(int argc, char **argv, const HlCommandEnv *env);

#endif /* HL_COMMANDS_JOBS_H */
