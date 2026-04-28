#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

fail=0

check_absent() {
    local pattern="$1"
    local message="$2"
    if rg -n "$pattern" AGENTS.md docs -g '*.md' >/tmp/ssot_lint_hits.txt 2>/dev/null; then
        echo "FAIL: $message"
        cat /tmp/ssot_lint_hits.txt
        fail=1
    fi
}

check_present() {
    local pattern="$1"
    local path="$2"
    local message="$3"
    if ! rg -n "$pattern" "$path" >/dev/null 2>&1; then
        echo "FAIL: $message"
        fail=1
    fi
}

# Stale MCP names that must not appear in active docs/policy.
check_absent 'mcp__sel4-autopilot__build_vm_minimal' "stale MCP build function found"
check_absent 'mcp__sel4-autopilot__test_vm_minimal' "stale MCP test function found"
check_absent 'mcp__sel4-autopilot__test_sel4_binary' "stale MCP test function found"
check_absent 'mcp__sel4-autopilot__build_sel4test' "MCP build function is not allowed for canonical build flow"

# Canonical hooks must exist.
check_present 'docs/agents/task-router.md' AGENTS.md "AGENTS.md must link to task router"
check_present 'autopilot --autopilot-dir' docs/agents/build-test-runbook.md "runbook must use autopilot CLI"
check_present 'Use Autopilot chains, not legacy profiles' docs/agents/build-test-runbook.md "runbook must define chain-based autopilot rule"

if [[ "$fail" -ne 0 ]]; then
    exit 1
fi

echo "SSOT lint passed"
