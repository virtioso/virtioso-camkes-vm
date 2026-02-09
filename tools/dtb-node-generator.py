#!/usr/bin/env python3
"""
DTB Node List Generator for CAmkES VM

Generates sorted, deduplicated dtb() entries for devices.camkes from a DTS source file.
Supports subtree inclusion, phandle following, and validation against a reference DTS.

Usage:
    ./dtb-node-generator.py --dts kernel/tools/dts/orinagx.dts --config .config \
        --device "/bus@0/ethernet@6800000:phandles" \
        --device "/bus@0/gpio@2200000" \
        --exclude "interrupt-controller" \
        --output devices.camkes.fragment
"""

import os
import sys
import argparse
import re
from typing import Dict, List, Optional, Set, Tuple, Any
from dataclasses import dataclass

# Add tools directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dts_utils import (
    load_dts,
    read_config_include_paths,
    get_default_include_paths,
    FDTHelper,
    compare_nodes,
    DTSProcessingError,
    DEFAULT_PHANDLE_PROPERTIES,
)


@dataclass
class DeviceSpec:
    """Specification for a device to include."""
    path: str
    include_subtree: bool = False
    follow_phandles: bool = False


@dataclass
class NodeEntry:
    """Entry for a node to be included in dtb()."""
    path: str
    address: Optional[int]
    source: str  # How this node was included (explicit, subtree, phandle)


def parse_device_spec(spec: str) -> DeviceSpec:
    """
    Parse a device specification string.

    Format: /path/to/device[:options]
    Options (comma-separated):
        - subtree: include all child nodes
        - phandles: follow phandle references
        - both: subtree and phandles

    Examples:
        /bus@0/serial@31d0000
        /bus@0/ethernet@6800000:phandles
        /bus@0/gpio@2200000:subtree,phandles
        /bus@0/mmc@3460000:both
    """
    if ':' in spec:
        path, options_str = spec.split(':', 1)
        options = options_str.lower().split(',')
    else:
        path = spec
        options = []

    include_subtree = 'subtree' in options or 'both' in options
    follow_phandles = 'phandles' in options or 'both' in options

    return DeviceSpec(
        path=path.strip(),
        include_subtree=include_subtree,
        follow_phandles=follow_phandles
    )


def get_parent_path(path: str) -> Optional[str]:
    """Get parent node path, or None if root."""
    if path == '/' or '/' not in path:
        return None
    return path.rsplit('/', 1)[0] or '/'


def collect_nodes(
    helper: FDTHelper,
    device_specs: List[DeviceSpec],
    exclude_patterns: List[str],
    include_parents: bool = True
) -> List[NodeEntry]:
    """
    Collect all nodes to include based on device specifications.

    Args:
        helper: FDTHelper for the FDT
        device_specs: List of device specifications
        exclude_patterns: List of patterns to exclude
        include_parents: If True, auto-include parent nodes of all collected nodes

    Returns:
        List of NodeEntry objects (deduplicated, unsorted)
    """
    entries: Dict[str, NodeEntry] = {}

    for spec in device_specs:
        node = helper.get_node_by_path(spec.path)
        if node is None:
            print(f"WARNING: Node not found: {spec.path}", file=sys.stderr)
            continue

        # Add the node itself
        addr = helper.get_reg_address(node)
        if spec.path not in entries:
            entries[spec.path] = NodeEntry(
                path=spec.path,
                address=addr,
                source="explicit"
            )

        # Add subtree if requested
        if spec.include_subtree:
            children = helper.get_all_children(node, recursive=True)
            for child in children:
                child_path = helper.get_node_path(child)
                if child_path not in entries:
                    child_addr = helper.get_reg_address(child)
                    entries[child_path] = NodeEntry(
                        path=child_path,
                        address=child_addr,
                        source=f"subtree of {spec.path}"
                    )

        # Follow phandles if requested
        if spec.follow_phandles:
            phandle_props = set(DEFAULT_PHANDLE_PROPERTIES)
            referenced = list(helper.follow_phandles(node, phandle_props))

            # Also follow phandles from children if subtree was requested
            if spec.include_subtree:
                children = helper.get_all_children(node, recursive=True)
                for child in children:
                    more_refs = helper.follow_phandles(child, phandle_props)
                    referenced.extend(more_refs)

            for ref_node in referenced:
                ref_path = helper.get_node_path(ref_node)
                if ref_path not in entries:
                    ref_addr = helper.get_reg_address(ref_node)
                    entries[ref_path] = NodeEntry(
                        path=ref_path,
                        address=ref_addr,
                        source=f"phandle from {spec.path}"
                    )

    # Add parent nodes if requested
    if include_parents:
        paths_to_add = list(entries.keys())
        for path in paths_to_add:
            parent = get_parent_path(path)
            while parent and parent != '/':
                if parent not in entries:
                    parent_node = helper.get_node_by_path(parent)
                    if parent_node:
                        parent_addr = helper.get_reg_address(parent_node)
                        entries[parent] = NodeEntry(
                            path=parent,
                            address=parent_addr,
                            source=f"parent of {path}"
                        )
                parent = get_parent_path(parent)

    # Filter out excluded patterns
    filtered_entries = {}
    for path, entry in entries.items():
        excluded = False
        for pattern in exclude_patterns:
            if pattern in path:
                excluded = True
                print(f"Excluding: {path} (matches '{pattern}')", file=sys.stderr)
                break
        if not excluded:
            filtered_entries[path] = entry

    return list(filtered_entries.values())


def sort_nodes_by_address(entries: List[NodeEntry]) -> List[NodeEntry]:
    """
    Sort nodes by physical address (required by capdl-loader).

    Nodes without addresses are placed at the end.
    """
    def sort_key(entry: NodeEntry) -> Tuple[int, str]:
        # Use a very large number for nodes without addresses
        addr = entry.address if entry.address is not None else 0xFFFFFFFFFFFFFFFF
        return (addr, entry.path)

    return sorted(entries, key=sort_key)


def generate_camkes_output(
    entries: List[NodeEntry],
    dts_path: str,
    include_paths: List[str],
    output_file: Optional[str] = None
) -> str:
    """
    Generate CAmkES dtb() output.

    Args:
        entries: Sorted list of node entries
        dts_path: Source DTS file path
        include_paths: Include paths used
        output_file: Optional output file path

    Returns:
        Generated CAmkES code
    """
    lines = []

    # Header comment
    lines.append(f"/* Auto-generated by dtb-node-generator.py")
    lines.append(f" * Source: {dts_path}")
    lines.append(f" * Include paths: {', '.join(include_paths[:2])}{'...' if len(include_paths) > 2 else ''}")
    lines.append(f" */")
    lines.append("")
    lines.append("vm0.dtb = dtb([")

    # Generate entries
    for i, entry in enumerate(entries):
        # Format address
        if entry.address is not None:
            addr_str = f"0x{entry.address:x}"
        else:
            addr_str = "no reg"

        # Add $ anchor for exact matching
        path_escaped = entry.path + "$"

        # Format entry
        comma = "," if i < len(entries) - 1 else ""
        line = f'    {{"path": "{path_escaped}"}}{comma}'

        # Add comment with address and source
        comment = f" /* {addr_str} - {entry.source} */"
        lines.append(line + comment)

    lines.append("]);")
    lines.append("")

    output = "\n".join(lines)

    if output_file:
        with open(output_file, 'w') as f:
            f.write(output)
        print(f"Output written to: {output_file}", file=sys.stderr)

    return output


def validate_against_reference(
    kernel_helper: FDTHelper,
    reference_helper: FDTHelper,
    entries: List[NodeEntry],
    ignore_props: Optional[Set[str]] = None,
    strict: bool = False
) -> Tuple[int, int, List[str]]:
    """
    Validate kernel DTS nodes against reference DTS.

    Args:
        kernel_helper: FDTHelper for kernel DTS
        reference_helper: FDTHelper for reference DTS
        entries: List of nodes to validate
        ignore_props: Properties to ignore in comparison
        strict: If True, treat differences as errors

    Returns:
        Tuple of (warnings, errors, messages)
    """
    warnings = 0
    errors = 0
    messages = []

    if ignore_props is None:
        ignore_props = {'phandle', 'linux,phandle', 'status'}

    messages.append("=== DTS Validation Report ===")
    messages.append(f"Validating {len(entries)} nodes for passthrough...\n")

    for entry in entries:
        kernel_node = kernel_helper.get_node_by_path(entry.path)
        reference_node = reference_helper.get_node_by_path(entry.path)

        if kernel_node is None:
            msg = f"[ERROR] {entry.path}: Node not found in kernel DTS"
            messages.append(msg)
            errors += 1
            continue

        if reference_node is None:
            msg = f"[INFO]  {entry.path}: Node not in reference (kernel-specific)"
            messages.append(msg)
            continue

        differences = compare_nodes(
            kernel_node, reference_node,
            kernel_helper, reference_helper,
            ignore_props
        )

        if differences:
            msg = f"[WARN]  {entry.path}"
            messages.append(msg)
            for diff in differences:
                messages.append(f"  - {diff}")
            warnings += len(differences)
        else:
            messages.append(f"[OK]    {entry.path}")

    messages.append(f"\nSummary: {len(entries)} nodes, {warnings} warnings, {errors} errors")

    return warnings, errors, messages


def main():
    parser = argparse.ArgumentParser(
        description='Generate CAmkES dtb() entries from DTS source file',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    # Generate dtb() entries for ethernet with phandle following
    %(prog)s --dts kernel/tools/dts/orinagx.dts --config .config \\
        --device "/bus@0/ethernet@6800000:phandles"

    # Include subtree and follow phandles
    %(prog)s --dts kernel/tools/dts/orinagx.dts --config .config \\
        --device "/bus@0/ethernet@6800000:both" \\
        --exclude "interrupt-controller"

    # Validate against reference DTS
    %(prog)s --dts kernel/tools/dts/orinagx.dts --config .config \\
        --reference /path/to/tegra234-p3737-0000+p3701-0000.dts \\
        --device "/bus@0/ethernet@6800000:phandles" \\
        --validate
"""
    )

    parser.add_argument('--dts', required=True,
                        help='Path to DTS source file')
    parser.add_argument('--config', '-c',
                        help='Path to .config file to read include paths')
    parser.add_argument('--include-path', '-I', action='append', default=[],
                        dest='include_paths',
                        help='Include path for cpp preprocessing (can specify multiple)')
    parser.add_argument('--device', '-d', action='append', default=[],
                        dest='devices',
                        help='Device to include (format: /path[:options])')
    parser.add_argument('--exclude', '-e', action='append', default=[],
                        dest='excludes',
                        help='Pattern to exclude from output')
    parser.add_argument('--output', '-o',
                        help='Output file (default: stdout)')
    parser.add_argument('--reference', '-r',
                        help='Reference DTS for validation')
    parser.add_argument('--validate', '-v', action='store_true',
                        help='Validate kernel DTS against reference')
    parser.add_argument('--strict', action='store_true',
                        help='Treat validation warnings as errors')
    parser.add_argument('--ignore-props',
                        help='Comma-separated list of properties to ignore in validation')
    parser.add_argument('--dry-run', action='store_true',
                        help='Show configuration without generating output')

    args = parser.parse_args()

    # Collect include paths
    include_paths = list(args.include_paths)
    if args.config:
        include_paths.extend(read_config_include_paths(args.config))
    include_paths.extend(get_default_include_paths(args.dts))

    # Remove duplicates while preserving order
    seen = set()
    unique_paths = []
    for p in include_paths:
        if p not in seen:
            seen.add(p)
            unique_paths.append(p)
    include_paths = unique_paths

    if args.dry_run:
        print(f"DTS file: {args.dts}")
        print(f"Include paths:")
        for p in include_paths:
            print(f"  {p}")
        print(f"Devices: {args.devices}")
        print(f"Excludes: {args.excludes}")
        if args.reference:
            print(f"Reference: {args.reference}")
        return 0

    if not args.devices:
        print("Error: No devices specified. Use --device to specify at least one device.", file=sys.stderr)
        return 1

    try:
        # Load kernel DTS
        print(f"Loading DTS: {args.dts}", file=sys.stderr)
        kernel_fdt = load_dts(args.dts, include_paths)
        kernel_helper = FDTHelper(kernel_fdt)

        # Parse device specifications
        device_specs = [parse_device_spec(d) for d in args.devices]

        # Collect nodes
        entries = collect_nodes(kernel_helper, device_specs, args.excludes)
        print(f"Collected {len(entries)} nodes", file=sys.stderr)

        # Sort by address
        entries = sort_nodes_by_address(entries)

        # Validation against reference (if requested)
        if args.reference and args.validate:
            print(f"Loading reference DTS: {args.reference}", file=sys.stderr)
            reference_fdt = load_dts(args.reference, include_paths)
            reference_helper = FDTHelper(reference_fdt)

            ignore_props = None
            if args.ignore_props:
                ignore_props = set(args.ignore_props.split(','))

            warnings, errors, messages = validate_against_reference(
                kernel_helper, reference_helper,
                entries, ignore_props, args.strict
            )

            for msg in messages:
                print(msg, file=sys.stderr)

            if args.strict and (warnings > 0 or errors > 0):
                print("\nValidation failed (strict mode)", file=sys.stderr)
                return 1

        # Generate output
        output = generate_camkes_output(entries, args.dts, include_paths, args.output)

        if not args.output:
            print(output)

        return 0

    except DTSProcessingError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
