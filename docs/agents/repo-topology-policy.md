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
