#!/usr/bin/env python3
import argparse
import json
import os
import re
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def _escape_mermaid(text: str) -> str:
    if text is None:
        return ""
    # Keep ASCII; replace newlines with <br/> for Mermaid.
    text = text.replace("\\", "\\\\")
    text = text.replace("\"", "'")
    text = text.replace("|", "/")
    text = text.replace("\n", "<br/>")
    return text


def _truncate(text: str, max_len: int) -> str:
    if text is None:
        return ""
    if len(text) <= max_len:
        return text
    return text[: max_len - 3] + "..."


def _safe_id(text: str) -> str:
    if not text:
        return "_"
    return re.sub(r"[^A-Za-z0-9_]", "_", text)


def _resolve_profiles_dir(arg: Optional[str]) -> Path:
    if arg:
        return Path(arg)
    autopilot_dir = os.environ.get("AUTOPILOT_DIR")
    if autopilot_dir:
        return Path(autopilot_dir) / "profiles"
    # Best-effort: resolve relative to this script (repo) location.
    here = Path(__file__).resolve()
    candidate = here.parent.parent.parent / "autopilot" / "profiles"
    if candidate.exists():
        return candidate
    # Fallback to cwd.
    return Path("autopilot") / "profiles"


def _resolve_out_dir(arg: Optional[str], profiles_dir: Path) -> Path:
    if arg:
        return Path(arg)
    # Default alongside AUTOPILOT_DIR if set.
    autopilot_dir = os.environ.get("AUTOPILOT_DIR")
    if autopilot_dir:
        return Path(autopilot_dir) / "diagrams"
    # Fallback to sibling of profiles dir.
    return profiles_dir.parent / "diagrams"


def _validate_chain(chain: dict) -> None:
    if "entry" not in chain:
        raise ValueError("chain entry missing")
    if "steps" not in chain:
        raise ValueError("chain steps missing")
    steps = chain["steps"]
    if chain["entry"] not in steps:
        raise ValueError("entry step not found in steps")
    for name, step in steps.items():
        if "type" not in step:
            raise ValueError(f"step {name} missing type")
        if "on_timeout" not in step and step["type"] not in ("pass", "fail"):
            raise ValueError(f"step {name} missing on_timeout")
        for outcome in step.get("outcomes", []):
            if "next" not in outcome:
                raise ValueError(f"step {name} outcome missing next")
            if outcome["next"] not in steps:
                raise ValueError(f"step {name} outcome target missing: {outcome['next']}")
        if "on_timeout" in step and step["on_timeout"] not in steps:
            raise ValueError(f"step {name} on_timeout target missing")
        if "on_error" in step and step["on_error"] not in steps:
            raise ValueError(f"step {name} on_error target missing")


def _format_kv(key: str, value: Optional[str], max_len: int) -> Optional[str]:
    if value is None:
        return None
    if isinstance(value, (list, dict)):
        value = json.dumps(value)
    value = str(value)
    value = _truncate(value, max_len)
    return f"{key}={value}"


def _step_fields(step: dict, max_len: int) -> List[str]:
    step_type = step.get("type", "")
    fields: List[str] = []
    if step_type == "map_source":
        fields.extend([
            _format_kv("source", step.get("source"), max_len),
            _format_kv("tty", step.get("tty"), max_len),
            _format_kv("log", step.get("log"), max_len),
            _format_kv("baud", step.get("baud"), max_len),
        ])
    elif step_type == "map_window":
        fields.extend([
            _format_kv("window", step.get("window"), max_len),
            _format_kv("source", step.get("source"), max_len),
            _format_kv("title", step.get("title"), max_len),
        ])
    elif step_type == "send_cmd":
        fields.extend([
            _format_kv("source", step.get("source"), max_len),
            _format_kv("cmd", step.get("cmd"), max_len),
        ])
    elif step_type == "boot_menu":
        fields.extend([
            _format_kv("source", step.get("source"), max_len),
            _format_kv("boot_option", step.get("boot_option"), max_len),
        ])
    elif step_type == "wait_pattern":
        fields.extend([
            _format_kv("source", step.get("source"), max_len),
            _format_kv("timeout_s", step.get("timeout_s"), max_len),
            _format_kv("pattern", step.get("pattern"), max_len),
        ])
    elif step_type in ("upload_kernel", "upload_efi"):
        fields.extend([
            _format_kv("target_user", step.get("target_user"), max_len),
            _format_kv("target_ip", step.get("target_ip"), max_len),
            _format_kv("target_path", step.get("target_path"), max_len),
        ])
    elif step_type == "reboot":
        fields.extend([
            _format_kv("method", step.get("method"), max_len),
            _format_kv("target_user", step.get("target_user"), max_len),
            _format_kv("target_ip", step.get("target_ip"), max_len),
        ])
    elif step_type == "interactive_console":
        sessions = step.get("sessions", [])
        summary = []
        for sess in sessions:
            name = sess.get("name")
            profile = sess.get("profile")
            if name and profile:
                summary.append(f"{name}/{profile}")
            elif name:
                summary.append(name)
        fields.append(_format_kv("sessions", ",".join(summary), max_len))
    elif step_type in ("fork", "join"):
        fields.append(_format_kv("chain", step.get("chain"), max_len))
    elif step_type == "analyze_logs":
        fields.append(_format_kv("command", step.get("command"), max_len))
    return [f for f in fields if f]


def _node_label(step_name: str, step: dict, detail: str, max_len: int) -> str:
    if detail == "minimal":
        return step_name
    lines = [step_name, f"type={step.get('type', '')}"]
    if detail == "verbose":
        lines.extend(_step_fields(step, max_len))
    return "<br/>".join(_escape_mermaid(l) for l in lines if l)


def _edge_label(outcome: dict, max_len: int) -> str:
    parts = []
    label = outcome.get("label")
    if label:
        parts.append(f"label={label}")
    source = outcome.get("source")
    if source:
        parts.append(f"src={source}")
    pattern = outcome.get("pattern")
    if pattern:
        parts.append(f"pat={_truncate(str(pattern), max_len)}")
    return _escape_mermaid(", ".join(parts))


def _render_chain(
    chain: dict,
    chain_name: str,
    detail: str,
    max_len: int,
    lines: List[str],
    all_nodes: List[str],
) -> None:
    _validate_chain(chain)
    steps = chain["steps"]
    subchains = chain.get("subchains", {})

    lines.append(f"subgraph { _safe_id(chain_name) }[{_escape_mermaid(chain_name)}]")
    start_id = _safe_id(f"{chain_name}_start")
    entry = chain["entry"]
    entry_id = _safe_id(f"{chain_name}__{entry}")
    lines.append(f"{start_id}((start))")
    lines.append(f"{start_id} --> {entry_id}")

    for step_name, step in steps.items():
        node_id = _safe_id(f"{chain_name}__{step_name}")
        label = _node_label(step_name, step, detail, max_len)
        lines.append(f"{node_id}[\"{label}\"]")
        all_nodes.append(node_id)

    for step_name, step in steps.items():
        node_id = _safe_id(f"{chain_name}__{step_name}")
        if step.get("type") == "fork":
            fork_chain = step.get("chain")
            if fork_chain and fork_chain in subchains:
                sub_name = f"{chain_name}.{fork_chain}"
                sub_start_id = _safe_id(f"{sub_name}_start")
                lines.append(f"{node_id} -.->|fork: { _escape_mermaid(fork_chain) }| {sub_start_id}")
        for outcome in step.get("outcomes", []):
            next_step = outcome.get("next")
            if not next_step:
                continue
            next_id = _safe_id(f"{chain_name}__{next_step}")
            label = _edge_label(outcome, max_len)
            if label:
                lines.append(f"{node_id} -->|{label}| {next_id}")
            else:
                lines.append(f"{node_id} --> {next_id}")
        if "on_timeout" in step:
            next_id = _safe_id(f"{chain_name}__{step['on_timeout']}")
            lines.append(f"{node_id} -.->|timeout| {next_id}")
        if "on_error" in step:
            next_id = _safe_id(f"{chain_name}__{step['on_error']}")
            lines.append(f"{node_id} -.->|error| {next_id}")

    for sub_name, subchain in subchains.items():
        _render_chain(subchain, f"{chain_name}.{sub_name}", detail, max_len, lines, all_nodes)

    lines.append("end")


class MissingChainError(ValueError):
    pass


def render_profile(profile_path: Path, detail: str = "verbose", max_len: int = 80) -> str:
    payload = json.loads(profile_path.read_text())
    chain = payload.get("chain")
    if not chain:
        raise MissingChainError(f"profile {profile_path.name} missing chain")

    lines: List[str] = ["flowchart TD"]
    all_nodes: List[str] = []
    _render_chain(chain, profile_path.stem, detail, max_len, lines, all_nodes)

    # Style pass/fail nodes if present.
    lines.append("classDef pass fill:#dff0d8,stroke:#3c763d,stroke-width:1px;")
    lines.append("classDef fail fill:#f2dede,stroke:#a94442,stroke-width:1px;")
    for node in all_nodes:
        if node.endswith("__pass"):
            lines.append(f"class {node} pass;")
        if node.endswith("__fail"):
            lines.append(f"class {node} fail;")

    return "\n".join(lines) + "\n"


def _write_docs(output_path: Path, diagrams: List[Tuple[str, str]]) -> None:
    lines = [
        "# Autopilot Chain Diagrams",
        "",
        "Regenerate with:",
        "",
        "```bash",
        "python3 tools/autopilot_chain_viz.py --all --docs",
        "```",
        "",
    ]
    for name, mermaid in diagrams:
        lines.append(f"## {name}")
        lines.append("")
        lines.append("```mermaid")
        lines.append(mermaid.rstrip())
        lines.append("```")
        lines.append("")
    output_path.write_text("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description="Render Autopilot chain diagrams to Mermaid.")
    parser.add_argument("--profile", help="Profile name without .json")
    parser.add_argument("--all", action="store_true", help="Render all profiles")
    parser.add_argument("--profiles-dir", help="Directory containing profile JSON files")
    parser.add_argument("--out-dir", help="Output directory for .mmd files")
    parser.add_argument(
        "--detail",
        choices=["verbose", "compact", "minimal"],
        default="verbose",
        help="Detail level for node labels",
    )
    parser.add_argument("--docs", action="store_true", help="Generate docs page with diagrams")
    parser.add_argument(
        "--max-len",
        type=int,
        default=80,
        help="Max length for patterns/commands in labels",
    )

    args = parser.parse_args()
    if not args.profile and not args.all:
        parser.error("Must specify --profile or --all")

    profiles_dir = _resolve_profiles_dir(args.profiles_dir)
    if not profiles_dir.exists():
        raise SystemExit(f"profiles directory not found: {profiles_dir}")

    out_dir = _resolve_out_dir(args.out_dir, profiles_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.all:
        profile_paths = sorted(profiles_dir.glob("*.json"))
    else:
        profile_paths = [profiles_dir / f"{args.profile}.json"]

    diagrams: List[Tuple[str, str]] = []
    errors: List[str] = []
    warnings: List[str] = []
    for profile_path in profile_paths:
        if not profile_path.exists():
            errors.append(f"missing profile: {profile_path}")
            continue
        try:
            mermaid = render_profile(profile_path, detail=args.detail, max_len=args.max_len)
            diagrams.append((profile_path.stem, mermaid))
            out_path = out_dir / f"{profile_path.stem}.mmd"
            out_path.write_text(mermaid)
        except MissingChainError as exc:
            if args.all:
                warnings.append(str(exc))
                continue
            errors.append(str(exc))
        except Exception as exc:
            errors.append(f"{profile_path.name}: {exc}")

    if args.docs:
        docs_path = Path(__file__).resolve().parent.parent / "docs" / "reference" / "autopilot-chain-diagrams.md"
        _write_docs(docs_path, diagrams)

    for warning in warnings:
        print(f"warning: {warning}")
    if errors:
        for err in errors:
            print(f"error: {err}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
