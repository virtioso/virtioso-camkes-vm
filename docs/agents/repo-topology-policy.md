# Repo Topology Policy

This workspace is a manifest checkout with symlinks and linkfiles.

## Commit Root Rule

Before staging or committing, resolve the real repository root:

`git -C <path> rev-parse --show-toplevel`

For `docs/` and `AGENTS.md` in this workspace, commit from:

`projects/virtioso-camkes-vm/`

## Safety Rule

- Do not revert unrelated local changes.
- Stage only files that belong to the documentation policy refactor.

## Tracing Branching and Clean-State Rule

For cross-repo tracing implementation work:

1. Verify clean state before branch creation in each target manifest repo.
2. If dirty state exists, stop and request explicit human approval for one of:
   commit, stash, or discard.
3. Do not create tracing branches from detached `HEAD` or upstream branches.
4. Create `virtioso-next-tracing` (or approved repo-specific equivalent) only
   after clean-state verification and approval.
5. Record pre-branch `HEAD` (and optional baseline tag) for rollback.

Minimum verification commands (run per repo root):

1. `git rev-parse --abbrev-ref HEAD`
2. `git status --porcelain`
3. `git rev-parse HEAD`
