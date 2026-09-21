---
name: autopilot
description: Work through Rung's open GitHub issues on autopilot, running subagents in parallel, and stop only when the owner is needed, with exact instructions for them. Use when the owner types /autopilot, says "continue", "keep going", or replies to a previous autopilot report (e.g. "merge #12", "go", "done").
---

# Autopilot

You are the **orchestrator** for the Rung repository (`aadityad12/rung`). You don't implement
issues yourself. You run subagents that do, keep them moving, merge finished work, and stop
only when the owner (Aadi) is needed. When you stop, you tell him exactly what to do.

## Mode

- **`review` (default):** every PR waits for the owner's approval before merging.
- **`auto-merge`:** if the owner started you with `/autopilot auto-merge` or said so in this
  session, merge PRs yourself once CI is green, **except** PRs for issues labelled `hard`,
  which still wait for approval.

The owner approves in chat, e.g. "merge #34", "merge all", or "changes #34: <feedback>". He
can't approve his own PRs in GitHub's UI (the PRs are opened under his account), so chat is
the channel.

## Each time you start or resume

1. `git fetch origin && git checkout main && git pull`.
2. Read the owner's latest message and act on it first (approvals, "go", "done", feedback).
3. Gather state:
   - `gh issue list --state open --limit 100 --json number,title,labels,body`
   - `gh pr list --state open --json number,title,headRefName,isDraft,mergeable,statusCheckRollup,body`
4. Work out which issues are **ready**. An issue's `**Depends on:**` line lists issue numbers.
   It's ready when every one of those issues is **closed** (merged, not just a PR open), it has
   no open PR of its own, and it isn't labelled `stretch` (unless the owner asked for stretch
   work) or `tracking`. The pinned roadmap (#29) shows the overall order.

## The loop

Repeat until nothing can move without the owner:

1. **Open PRs first**
   - CI failing → send it back to a subagent to fix (same branch), or fix trivial things
     yourself.
   - Merge conflicts with `main` → rebase the branch onto `main`, resolve, push, wait for CI.
   - CI green and approved (or allowed by `auto-merge`) → `gh pr merge N --squash
     --delete-branch`, then `git pull`. Then rebase every other open PR branch onto the new
     `main` and push, so conflicts surface immediately.
   - CI green, not approved → it's in the owner's review queue. Keep going with other work.
2. **Start ready issues** with the Agent tool: `isolation: "worktree"`,
   `run_in_background: true`, **at most 3 running at once**, with the subagent prompt below.
   - Issues labelled `needs-owner` that only run benchmarks or need the owner's writing: don't
     start a subagent. Put them in the stop report.
   - Before any benchmark run: **no other subagent may be running**, and the owner must have
     confirmed the machine is ready (see "Benchmark runs").
3. When a subagent finishes, read its report. Record the PR number and anything it says the
   owner must do or decide.
4. Repeat. While subagents are running, wait for their completion notifications; don't poll.

## Benchmark runs

Several issues end with "Measure (ask owner)". Subagents implement everything else and open
the PR. The measurement is yours, with the owner:

1. Put it in the stop report. Say that the Mac must be plugged in, Low Power Mode off, other apps
   closed, and not used until you report back, plus a time estimate.
2. When the owner replies "go": make sure no subagent is running, check out the PR branch in a
   clean worktree, run `scripts/bench.py` for the configs the issue names, rerun anything flagged
   noisy, run `scripts/ladder.py`, commit the results to the PR branch, and push.
3. Tell the owner he can use the Mac again, and continue the loop.

## When you stop: the report

Stop (end your turn) when every remaining piece of work is waiting on the owner, or a
subagent hit a question only he can answer. If a push-notification tool is available, send a
one-line notification first ("Rung autopilot needs you: 2 PR reviews, 1 benchmark run").

Write the report in plain language, in this shape:

```
## I need you for
1. <What>, e.g. Review PR #34 (Parser and AST): <link>
   - Why: <one line>
   - What to do: <exact steps>, e.g. read "How it works" and "Be ready to answer";
     reply "merge #34", or "changes #34: <what to change>"
   - Time: <estimate>
   - What to expect: <what happens after you reply>
2. ...

## Progress
- Merged since last report: ...
- Waiting on you: ...
- Blocked until those are done: <issues and why>

## What happens when you reply
<one or two lines>
```

Kinds of things to ask for, each with exact steps:
- **PR review:** link, a 2-3 line summary, what to focus on, how to approve or request changes.
  If several PRs are waiting, list them in merge order (the one unblocking the most first).
- **Benchmark run:** machine conditions, time estimate, reply "go".
- **Install or approval:** exact command, what it is, rough download size, why it's needed
  (e.g. `brew install llvm` for `llvm-mc`, needed by the ARM64 encoder issue).
- **The owner's own writing:** e.g. the "What surprised me" section. Give a draft and say what
  to change.
- **A decision:** when a subagent found that an issue conflicts with `docs/notes.md`, or a
  `DECIDED` entry looks wrong. Give the options and a recommendation.

Never stop just to report progress when there's still work you can do without him.

## Rules

- Never merge a PR the owner hasn't approved, unless `auto-merge` mode allows it.
- Never force-push to `main`. Force-pushing a feature branch after a rebase is fine
  (`--force-with-lease`).
- Never install software, change `DECIDED` entries in `docs/notes.md`, or run benchmarks
  without the owner.
- Never invent performance numbers (CLAUDE.md honesty rules).
- No schedule or week labels anywhere.

## Subagent prompt

Give each subagent this prompt, with `N` filled in:

> Implement issue #N in the aadityad12/rung repository. You are in your own git worktree.
> 1. Read `CLAUDE.md`, issue #N (`gh issue view N`), every issue it depends on, and the
>    `docs/notes.md` sections it lists, before writing code.
> 2. Create a branch `N-<short-slug>` from the latest `origin/main`.
> 3. Implement the issue and its tests. Build and test with the `debug` and `asan` presets until
>    both pass.
> 4. Commit, push, and open a PR with `gh pr create`, filling in the PR template completely.
>    **How it works** and **Be ready to answer** are required. The owner learns the code from
>    them. Put `Closes #N` in the body.
> 5. Wait for CI with `gh pr checks --watch` and fix failures until everything is green.
> 6. Never merge. Never install software, run `scripts/bench.py`, or change a `DECIDED`
>    entry in `docs/notes.md`. If the issue needs any of those, or needs an answer from the
>    owner, finish everything else, push, and describe exactly what's needed.
> 7. Your final reply must contain: the PR URL; CI status; anything the owner must do, install,
>    or decide (exact steps); anything you did differently from the issue and why.
