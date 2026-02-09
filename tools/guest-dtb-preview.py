#!/usr/bin/env python3
"""
Guest DTB Preview Generator for CAmkES VM

Generates a preview of the guest DTB at build time, replicating what libfdtgen
would produce at runtime. This enables debugging DTB issues without running
on target hardware.

Usage:
    ./guest-dtb-preview.py --dts kernel/tools/dts/orinagx.dts --config .config \
        --devices-camkes apps/Arm/vm_minimal/orinagx/devices.camkes \
        --output-dts guest-preview.dts
"""

import os
import sys
import re
import argparse
import tempfile
import subprocess
from typing import Dict, List, Optional, Set, Any, Tuple
from dataclasses import dataclass
import copy

# Add tools directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dts_utils import (
    load_dts,
    compile_dts_to_dtb,
    dtb_to_dts,
    read_config_include_paths,
    get_default_include_paths,
    FDTHelper,
    DTSProcessingError,
    DEFAULT_PHANDLE_PROPERTIES,
)

try:
    import pyfdt.pyfdt as pyfdt
except ImportError:
    from pyfdt import pyfdt


def parse_devices_camkes(camkes_path: str) -> List[str]:
    """
    Parse a devices.camkes file to extract dtb() query paths.

    Args:
        camkes_path: Path to devices.camkes file

    Returns:
        List of paths from dtb() entries (without $ anchors)
    """
    paths = []

    with open(camkes_path, 'r') as f:
        content = f.read()

    # Find dtb([...]) blocks
    # Pattern matches: {"path": "/some/path$"}
    pattern = r'\{"path"\s*:\s*"([^"]+)"\s*\}'

    for match in re.finditer(pattern, content):
        path = match.group(1)
        # Remove $ anchor if present
        if path.endswith('$'):
            path = path[:-1]
        paths.append(path)

    return paths


def collect_nodes_from_paths(helper: FDTHelper, paths: List[str]) -> Dict[str, pyfdt.FdtNode]:
    """
    Collect FDT nodes matching the given paths.

    Args:
        helper: FDTHelper for the FDT
        paths: List of paths to collect

    Returns:
        Dict mapping path to node
    """
    nodes = {}

    for path in paths:
        node = helper.get_node_by_path(path)
        if node:
            nodes[path] = node
        else:
            print(f"WARNING: Node not found: {path}", file=sys.stderr)

    return nodes


def keep_phandle_dependencies(
    helper: FDTHelper,
    nodes: Dict[str, pyfdt.FdtNode],
    phandle_props: Optional[Set[str]] = None
) -> Dict[str, pyfdt.FdtNode]:
    """
    Add phandle dependencies for nodes (like libfdtgen does).

    Args:
        helper: FDTHelper for the FDT
        nodes: Initial set of nodes to keep
        phandle_props: Phandle properties to follow

    Returns:
        Extended dict including phandle dependencies
    """
    if phandle_props is None:
        phandle_props = set(DEFAULT_PHANDLE_PROPERTIES)

    result = dict(nodes)

    # Keep adding dependencies until no new nodes are found
    changed = True
    while changed:
        changed = False
        for path, node in list(result.items()):
            refs = helper.follow_phandles(node, phandle_props, max_depth=1)
            for ref_node in refs:
                ref_path = helper.get_node_path(ref_node)
                if ref_path not in result:
                    result[ref_path] = ref_node
                    changed = True

    return result


def generate_summary(
    helper: FDTHelper,
    nodes: Dict[str, pyfdt.FdtNode]
) -> List[str]:
    """
    Generate a summary of nodes to be included in guest DTB.

    Args:
        helper: FDTHelper for the FDT
        nodes: Nodes to include

    Returns:
        List of summary lines
    """
    lines = []
    lines.append("=== Guest DTB Preview Summary ===\n")
    lines.append(f"Total nodes: {len(nodes)}\n")

    # Sort by path
    sorted_paths = sorted(nodes.keys())

    for path in sorted_paths:
        node = nodes[path]
        addr = helper.get_reg_address(node)
        addr_str = f"0x{addr:x}" if addr is not None else "no reg"

        # Get compatible property if present
        compat = helper.get_node_property(node, 'compatible')
        if compat and hasattr(compat, 'strings'):
            compat_str = compat.strings[0] if compat.strings else ""
        else:
            compat_str = ""

        lines.append(f"  {path}")
        if compat_str:
            lines.append(f"    compatible: {compat_str}")
        lines.append(f"    address: {addr_str}")

    return lines


def get_ancestors(path: str) -> List[str]:
    """Get all ancestor paths for a given path."""
    ancestors = []
    parts = path.split('/')
    for i in range(1, len(parts)):
        ancestor = '/'.join(parts[:i]) or '/'
        if ancestor and ancestor != '/':
            ancestors.append(ancestor)
    return ancestors


def compute_paths_to_keep(keep_paths: Set[str]) -> Set[str]:
    """
    Compute all paths that need to be kept, including ancestors.

    Args:
        keep_paths: Set of paths explicitly requested

    Returns:
        Set of all paths to keep (including ancestors)
    """
    result = set(keep_paths)

    # Add all ancestors
    for path in keep_paths:
        for ancestor in get_ancestors(path):
            result.add(ancestor)

    # Always keep root
    result.add('/')

    return result


def clone_property(prop: pyfdt.FdtProperty) -> pyfdt.FdtProperty:
    """Deep clone an FDT property."""
    if isinstance(prop, pyfdt.FdtPropertyStrings):
        return pyfdt.FdtPropertyStrings(prop.get_name(), prop.strings)
    elif isinstance(prop, pyfdt.FdtPropertyWords):
        return pyfdt.FdtPropertyWords(prop.get_name(), list(prop))
    elif isinstance(prop, pyfdt.FdtPropertyBytes):
        return pyfdt.FdtPropertyBytes(prop.get_name(), list(prop))
    else:
        # Generic property - copy raw data
        return pyfdt.FdtProperty(prop.get_name())


def clone_node_shallow(node: pyfdt.FdtNode) -> pyfdt.FdtNode:
    """Clone a node with its properties but without children."""
    new_node = pyfdt.FdtNode(node.get_name())

    # Copy all properties
    for item in node.subdata:
        if isinstance(item, pyfdt.FdtProperty):
            new_node.append(clone_property(item))

    return new_node


def filter_fdt_recursive(
    source_node: pyfdt.FdtNode,
    current_path: str,
    paths_to_keep: Set[str],
    helper: FDTHelper
) -> Optional[pyfdt.FdtNode]:
    """
    Recursively filter an FDT node tree.

    Args:
        source_node: Source node to filter
        current_path: Current path in the tree
        paths_to_keep: Set of paths to keep
        helper: FDTHelper for path resolution

    Returns:
        Filtered node or None if node should be removed
    """
    # Check if this path or any descendant should be kept
    dominated = False
    for keep_path in paths_to_keep:
        if keep_path == current_path or keep_path.startswith(current_path + '/'):
            dominated = True
            break

    if not dominated:
        return None

    # Clone this node (without children)
    new_node = clone_node_shallow(source_node)

    # Recursively process children
    for item in source_node.subdata:
        if isinstance(item, pyfdt.FdtNode):
            child_name = item.get_name()
            if current_path == '/':
                child_path = '/' + child_name
            else:
                child_path = current_path + '/' + child_name

            filtered_child = filter_fdt_recursive(item, child_path, paths_to_keep, helper)
            if filtered_child is not None:
                new_node.append(filtered_child)

    return new_node


def filter_fdt_to_nodes(
    source_fdt: pyfdt.Fdt,
    keep_paths: Set[str],
    helper: FDTHelper
) -> pyfdt.Fdt:
    """
    Create a filtered FDT containing only specified nodes.

    Args:
        source_fdt: Source FDT
        keep_paths: Set of paths to keep
        helper: FDTHelper for the source FDT

    Returns:
        Filtered FDT
    """
    # Compute all paths including ancestors
    paths_to_keep = compute_paths_to_keep(keep_paths)

    # Get the root node
    root_node = source_fdt.get_rootnode()

    # Filter the tree
    filtered_root = filter_fdt_recursive(root_node, '/', paths_to_keep, helper)

    if filtered_root is None:
        raise DTSProcessingError("Filtered FDT has no root node")

    # Create new FDT with filtered root
    # pyfdt doesn't have a clean way to create an FDT from scratch,
    # so we'll modify the source and rebuild
    new_fdt = pyfdt.Fdt()
    new_fdt.add_rootnode(filtered_root)

    # Copy reserve entries if they exist
    if hasattr(source_fdt, 'reserve_entries'):
        new_fdt.add_reserve_entries(source_fdt.reserve_entries)

    return new_fdt


def write_filtered_dts(
    source_fdt: pyfdt.Fdt,
    keep_paths: Set[str],
    helper: FDTHelper,
    output_path: str
) -> None:
    """
    Write a filtered DTS file containing only specified nodes.

    Args:
        source_fdt: Source FDT
        keep_paths: Set of paths to keep
        helper: FDTHelper for the source FDT
        output_path: Output DTS file path
    """
    # Filter the FDT
    filtered_fdt = filter_fdt_to_nodes(source_fdt, keep_paths, helper)

    # Write to temporary DTB
    with tempfile.NamedTemporaryFile(suffix='.dtb', delete=False) as tmp:
        tmp_dtb = tmp.name
        tmp.write(filtered_fdt.to_dtb())

    try:
        # Convert DTB to DTS using dtc
        dtb_to_dts(tmp_dtb, output_path)
    finally:
        if os.path.exists(tmp_dtb):
            os.unlink(tmp_dtb)


def main():
    parser = argparse.ArgumentParser(
        description='Generate guest DTB preview from DTS and devices.camkes',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    # Generate preview with summary
    %(prog)s --dts kernel/tools/dts/orinagx.dts --config .config \\
        --devices-camkes apps/Arm/vm_minimal/orinagx/devices.camkes \\
        --summary

    # Generate filtered DTS output
    %(prog)s --dts kernel/tools/dts/orinagx.dts --config .config \\
        --devices-camkes apps/Arm/vm_minimal/orinagx/devices.camkes \\
        --output-dts guest-preview.dts

    # List nodes from devices.camkes
    %(prog)s --devices-camkes apps/Arm/vm_minimal/orinagx/devices.camkes \\
        --list-paths
"""
    )

    parser.add_argument('--dts',
                        help='Path to DTS source file')
    parser.add_argument('--config', '-c',
                        help='Path to .config file to read include paths')
    parser.add_argument('--include-path', '-I', action='append', default=[],
                        dest='include_paths',
                        help='Include path for cpp preprocessing')
    parser.add_argument('--devices-camkes', '-d',
                        help='Path to devices.camkes file')
    parser.add_argument('--platform', '-p',
                        help='Platform name for customization hooks')
    parser.add_argument('--output-dts', '-o',
                        help='Output DTS file (filtered)')
    parser.add_argument('--output-dtb',
                        help='Output DTB file (filtered)')
    parser.add_argument('--summary', '-s', action='store_true',
                        help='Print summary of included nodes')
    parser.add_argument('--list-paths', action='store_true',
                        help='Just list paths from devices.camkes and exit')
    parser.add_argument('--keep-phandles', action='store_true', default=True,
                        help='Keep phandle dependencies (default: true)')

    args = parser.parse_args()

    # List paths mode
    if args.list_paths:
        if not args.devices_camkes:
            print("Error: --devices-camkes required for --list-paths", file=sys.stderr)
            return 1
        paths = parse_devices_camkes(args.devices_camkes)
        print(f"Found {len(paths)} paths in {args.devices_camkes}:")
        for path in paths:
            print(f"  {path}")
        return 0

    # Full mode requires DTS
    if not args.dts:
        print("Error: --dts required", file=sys.stderr)
        return 1

    if not args.devices_camkes:
        print("Error: --devices-camkes required", file=sys.stderr)
        return 1

    # Collect include paths
    include_paths = list(args.include_paths)
    if args.config:
        include_paths.extend(read_config_include_paths(args.config))
    include_paths.extend(get_default_include_paths(args.dts))

    # Remove duplicates
    seen = set()
    unique_paths = []
    for p in include_paths:
        if p not in seen:
            seen.add(p)
            unique_paths.append(p)
    include_paths = unique_paths

    try:
        # Parse devices.camkes
        print(f"Parsing devices.camkes: {args.devices_camkes}", file=sys.stderr)
        dtb_paths = parse_devices_camkes(args.devices_camkes)
        print(f"Found {len(dtb_paths)} dtb() entries", file=sys.stderr)

        # Load DTS
        print(f"Loading DTS: {args.dts}", file=sys.stderr)
        fdt = load_dts(args.dts, include_paths)
        helper = FDTHelper(fdt)

        # Collect nodes from paths
        nodes = collect_nodes_from_paths(helper, dtb_paths)
        print(f"Collected {len(nodes)} nodes from DTS", file=sys.stderr)

        # Add phandle dependencies
        if args.keep_phandles:
            original_count = len(nodes)
            nodes = keep_phandle_dependencies(helper, nodes)
            added = len(nodes) - original_count
            if added > 0:
                print(f"Added {added} phandle dependencies", file=sys.stderr)

        # Generate summary
        if args.summary or (not args.output_dts and not args.output_dtb):
            summary = generate_summary(helper, nodes)
            for line in summary:
                print(line)

        # Get the set of paths to keep
        keep_paths = set(nodes.keys())

        # Generate output DTS (filtered)
        if args.output_dts:
            print(f"Filtering DTS to {len(nodes)} nodes...", file=sys.stderr)
            write_filtered_dts(fdt, keep_paths, helper, args.output_dts)
            print(f"Filtered DTS written to: {args.output_dts}", file=sys.stderr)

        # Generate output DTB (filtered)
        if args.output_dtb:
            print(f"Filtering DTB to {len(nodes)} nodes...", file=sys.stderr)
            filtered_fdt = filter_fdt_to_nodes(fdt, keep_paths, helper)
            with open(args.output_dtb, 'wb') as f:
                f.write(filtered_fdt.to_dtb())
            print(f"Filtered DTB written to: {args.output_dtb}", file=sys.stderr)

        return 0

    except DTSProcessingError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1


if __name__ == '__main__':
    sys.exit(main())
