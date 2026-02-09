#!/usr/bin/env python3
"""
DTS Processing Utilities for CAmkES VM Device Tree Tooling

This module provides shared functionality for processing Device Tree Source (DTS)
files, including preprocessing with cpp, compilation with dtc, and parsing with pyfdt.

Dependencies:
    - pyfdt: Python FDT (Flattened Device Tree) library
    - cpp: C preprocessor (typically from gcc)
    - dtc: Device Tree Compiler
"""

import os
import re
import sys
import shutil
import tempfile
import subprocess
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Set, Any

# Try to import pyfdt from CAmkES location
CAMKES_PARSER_PATH = os.path.join(
    os.path.dirname(__file__),
    '..', '..', 'camkes-tool', 'camkes', 'parser'
)
if os.path.exists(CAMKES_PARSER_PATH):
    sys.path.insert(0, CAMKES_PARSER_PATH)

try:
    import pyfdt.pyfdt as pyfdt
except ImportError:
    # Try alternate import path
    try:
        from pyfdt import pyfdt
    except ImportError:
        print("Error: pyfdt library not found. Install via pip or ensure CAmkES is available.", file=sys.stderr)
        sys.exit(1)


# Default phandle properties to follow (same as libfdtgen)
DEFAULT_PHANDLE_PROPERTIES = [
    # Simple phandle (single value)
    "phy-handle",
    "next-level-cache",
    "shmem",
    # Phandle with cells
    "clocks",
    "power-domains",
    "mboxes",
    "resets",
    "dmas",
    "phys",
]

# Mapping of phandle property to its cells property
PHANDLE_CELLS_MAPPING = {
    "clocks": "#clock-cells",
    "power-domains": "#power-domain-cells",
    "mboxes": "#mbox-cells",
    "resets": "#reset-cells",
    "dmas": "#dma-cells",
    "phys": "#phy-cells",
}

# Properties that should NOT be followed (cause GIC/interrupt-controller inclusion)
EXCLUDED_PHANDLE_PROPERTIES = [
    "interrupt-parent",
    "interrupts-extended",
]


class DTSProcessingError(Exception):
    """Exception raised for DTS processing errors."""
    pass


def find_cpp() -> str:
    """Find the C preprocessor executable."""
    # Try common names
    for name in ['cpp', 'aarch64-linux-gnu-cpp', 'arm-linux-gnueabihf-cpp']:
        path = shutil.which(name)
        if path:
            return path
    raise DTSProcessingError("C preprocessor (cpp) not found in PATH")


def find_dtc() -> str:
    """Find the device tree compiler executable."""
    path = shutil.which('dtc')
    if path:
        return path
    raise DTSProcessingError("Device tree compiler (dtc) not found in PATH")


def read_config_include_paths(config_path: str) -> List[str]:
    """
    Extract dt-bindings include paths from a .config file.

    Looks for:
    - CONFIG_DT_BINDINGS_PATH
    - KERNEL_DTS_INCLUDE_PATH

    Args:
        config_path: Path to .config file

    Returns:
        List of include paths found
    """
    include_paths = []

    if not os.path.exists(config_path):
        return include_paths

    patterns = [
        r'^CONFIG_DT_BINDINGS_PATH="(.+)"',
        r'^KERNEL_DTS_INCLUDE_PATH="(.+)"',
    ]

    with open(config_path, 'r') as f:
        for line in f:
            for pattern in patterns:
                match = re.match(pattern, line.strip())
                if match:
                    path = match.group(1)
                    if os.path.exists(path):
                        include_paths.append(path)

    return include_paths


def get_default_include_paths(dts_path: str) -> List[str]:
    """
    Get default include paths based on DTS file location.

    Args:
        dts_path: Path to DTS source file

    Returns:
        List of default include paths
    """
    paths = []
    dts_dir = os.path.dirname(os.path.abspath(dts_path))

    # Add DTS directory itself
    paths.append(dts_dir)

    # Try to find kernel include directory
    # Walk up looking for 'include' directory with dt-bindings
    current = dts_dir
    for _ in range(10):
        include_dir = os.path.join(current, 'include')
        dt_bindings = os.path.join(include_dir, 'dt-bindings')
        if os.path.isdir(dt_bindings):
            paths.append(include_dir)
            break
        parent = os.path.dirname(current)
        if parent == current:
            break
        current = parent

    return paths


def preprocess_dts(dts_path: str, include_paths: List[str], output_path: Optional[str] = None) -> str:
    """
    Preprocess DTS file with C preprocessor to resolve #include directives.

    Args:
        dts_path: Path to input DTS file
        include_paths: List of include directories for cpp
        output_path: Optional output path (uses temp file if not provided)

    Returns:
        Path to preprocessed DTS file

    Raises:
        DTSProcessingError: If preprocessing fails
    """
    cpp = find_cpp()

    if output_path is None:
        fd, output_path = tempfile.mkstemp(suffix='.dts.pp')
        os.close(fd)

    # Build cpp command
    cmd = [cpp, '-nostdinc', '-undef', '-x', 'assembler-with-cpp']

    for path in include_paths:
        cmd.extend(['-I', path])

    cmd.extend(['-o', output_path, dts_path])

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        raise DTSProcessingError(f"cpp preprocessing failed:\n{e.stderr}")

    return output_path


def compile_dts_to_dtb(dts_path: str, include_paths: List[str], output_path: Optional[str] = None) -> str:
    """
    Preprocess and compile DTS file to DTB.

    Args:
        dts_path: Path to input DTS file
        include_paths: List of include directories for cpp
        output_path: Optional output path for DTB (uses temp file if not provided)

    Returns:
        Path to compiled DTB file

    Raises:
        DTSProcessingError: If compilation fails
    """
    dtc = find_dtc()

    # First preprocess
    pp_path = preprocess_dts(dts_path, include_paths)

    try:
        if output_path is None:
            fd, output_path = tempfile.mkstemp(suffix='.dtb')
            os.close(fd)

        # Compile with dtc
        cmd = [dtc, '-I', 'dts', '-O', 'dtb', '-o', output_path, pp_path]

        try:
            result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        except subprocess.CalledProcessError as e:
            raise DTSProcessingError(f"dtc compilation failed:\n{e.stderr}")

        return output_path
    finally:
        # Clean up preprocessed file
        if os.path.exists(pp_path):
            os.unlink(pp_path)


def dtb_to_dts(dtb_path: str, output_path: Optional[str] = None) -> str:
    """
    Decompile DTB to DTS format.

    Args:
        dtb_path: Path to input DTB file
        output_path: Optional output path (uses temp file if not provided)

    Returns:
        Path to decompiled DTS file

    Raises:
        DTSProcessingError: If decompilation fails
    """
    dtc = find_dtc()

    if output_path is None:
        fd, output_path = tempfile.mkstemp(suffix='.dts')
        os.close(fd)

    cmd = [dtc, '-I', 'dtb', '-O', 'dts', '-o', output_path, dtb_path]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        raise DTSProcessingError(f"dtc decompilation failed:\n{e.stderr}")

    return output_path


def parse_dtb(dtb_path: str) -> pyfdt.Fdt:
    """
    Parse DTB file with pyfdt.

    Args:
        dtb_path: Path to DTB file

    Returns:
        Parsed FDT object

    Raises:
        DTSProcessingError: If parsing fails
    """
    try:
        with open(dtb_path, 'rb') as f:
            return pyfdt.FdtBlobParse(f).to_fdt()
    except Exception as e:
        raise DTSProcessingError(f"Failed to parse DTB: {e}")


def load_dts(dts_path: str, include_paths: List[str]) -> pyfdt.Fdt:
    """
    Load and parse DTS file (preprocess, compile, parse).

    Args:
        dts_path: Path to DTS source file
        include_paths: Include paths for preprocessing

    Returns:
        Parsed FDT object
    """
    dtb_path = compile_dts_to_dtb(dts_path, include_paths)
    try:
        return parse_dtb(dtb_path)
    finally:
        if os.path.exists(dtb_path):
            os.unlink(dtb_path)


class FDTHelper:
    """Helper class for working with parsed FDT data."""

    def __init__(self, fdt: pyfdt.Fdt):
        """
        Initialize helper with parsed FDT.

        Args:
            fdt: Parsed FDT object from pyfdt (result of FdtBlobParse.to_fdt())
        """
        self.fdt = fdt
        self._phandle_index: Optional[Dict[int, pyfdt.FdtNode]] = None
        self._path_index: Optional[Dict[str, pyfdt.FdtNode]] = None

    def get_root(self) -> pyfdt.FdtNode:
        """Get the root node of the FDT."""
        return self.fdt.get_rootnode()

    def build_phandle_index(self) -> Dict[int, pyfdt.FdtNode]:
        """
        Build index mapping phandle values to nodes.

        Returns:
            Dict mapping phandle value to FdtNode
        """
        if self._phandle_index is not None:
            return self._phandle_index

        self._phandle_index = {}
        root = self.get_root()

        def walk(node: pyfdt.FdtNode, path: str):
            for sub in node.subdata:
                if isinstance(sub, pyfdt.FdtPropertyWords) and sub.get_name() == 'phandle':
                    if sub.words:
                        self._phandle_index[sub.words[0]] = node
                elif isinstance(sub, pyfdt.FdtNode):
                    subpath = f"{path}/{sub.get_name()}" if path else sub.get_name()
                    walk(sub, subpath)

        walk(root, '')
        return self._phandle_index

    def build_path_index(self) -> Dict[str, pyfdt.FdtNode]:
        """
        Build index mapping paths to nodes.

        Returns:
            Dict mapping path string to FdtNode
        """
        if self._path_index is not None:
            return self._path_index

        self._path_index = {'/': self.get_root()}
        root = self.get_root()

        def walk(node: pyfdt.FdtNode, path: str):
            for sub in node.subdata:
                if isinstance(sub, pyfdt.FdtNode):
                    subpath = f"{path}/{sub.get_name()}"
                    self._path_index[subpath] = sub
                    walk(sub, subpath)

        walk(root, '')
        return self._path_index

    def get_node_by_phandle(self, phandle: int) -> Optional[pyfdt.FdtNode]:
        """Get node by phandle value."""
        return self.build_phandle_index().get(phandle)

    def get_node_by_path(self, path: str) -> Optional[pyfdt.FdtNode]:
        """Get node by path."""
        return self.build_path_index().get(path)

    def get_node_path(self, node: pyfdt.FdtNode) -> str:
        """
        Get the full path of a node.

        Args:
            node: FDT node

        Returns:
            Full path string (e.g., "/bus@0/serial@31d0000")
        """
        # Build path index to create reverse mapping
        path_index = self.build_path_index()
        for path, n in path_index.items():
            if n is node:
                return path
        return '/'

    def get_node_property(self, node: pyfdt.FdtNode, name: str) -> Optional[Any]:
        """
        Get a property from a node.

        Args:
            node: FDT node
            name: Property name

        Returns:
            Property object or None if not found
        """
        for sub in node.subdata:
            if hasattr(sub, 'get_name') and sub.get_name() == name:
                if not isinstance(sub, pyfdt.FdtNode):
                    return sub
        return None

    def get_reg_address(self, node: pyfdt.FdtNode) -> Optional[int]:
        """
        Extract the first physical address from a node's reg property.

        Args:
            node: FDT node

        Returns:
            First physical address or None if no reg property
        """
        reg_prop = self.get_node_property(node, 'reg')
        if reg_prop is None:
            return None

        if isinstance(reg_prop, pyfdt.FdtPropertyWords):
            words = reg_prop.words
            if not words:
                return None

            # Determine address-cells (default 2 for 64-bit)
            # For simplicity, assume 2-cell addresses
            if len(words) >= 2:
                return (words[0] << 32) | words[1]
            elif len(words) >= 1:
                return words[0]

        return None

    def get_all_children(self, node: pyfdt.FdtNode, recursive: bool = True) -> List[pyfdt.FdtNode]:
        """
        Get all child nodes.

        Args:
            node: Parent FDT node
            recursive: If True, include all descendants

        Returns:
            List of child nodes
        """
        children = []
        for sub in node.subdata:
            if isinstance(sub, pyfdt.FdtNode):
                children.append(sub)
                if recursive:
                    children.extend(self.get_all_children(sub, recursive=True))
        return children

    def get_cells_count(self, node: pyfdt.FdtNode, cells_prop: str) -> int:
        """
        Get the #xxx-cells value from a node.

        Args:
            node: FDT node
            cells_prop: Property name (e.g., "#clock-cells")

        Returns:
            Cells count, defaults to 0 if not found
        """
        prop = self.get_node_property(node, cells_prop)
        if prop is None:
            return 0
        if isinstance(prop, pyfdt.FdtPropertyWords) and prop.words:
            return prop.words[0]
        return 0

    def extract_phandles_from_property(self, node: pyfdt.FdtNode, prop_name: str) -> List[int]:
        """
        Extract phandle values from a property, handling cells-based formats.

        Args:
            node: FDT node containing the property
            prop_name: Property name

        Returns:
            List of phandle values
        """
        phandles = []
        prop = self.get_node_property(node, prop_name)

        if prop is None:
            return phandles

        if not isinstance(prop, pyfdt.FdtPropertyWords) or not prop.words:
            return phandles

        words = prop.words
        cells_prop = PHANDLE_CELLS_MAPPING.get(prop_name)

        if cells_prop:
            # Property has cells - parse phandle + skip cells
            idx = 0
            while idx < len(words):
                phandle = words[idx]
                phandles.append(phandle)
                ref_node = self.get_node_by_phandle(phandle)
                cells = self.get_cells_count(ref_node, cells_prop) if ref_node else 0
                idx += 1 + cells
        else:
            # Simple phandle (single value)
            phandles.append(words[0])

        return phandles

    def follow_phandles(
        self,
        node: pyfdt.FdtNode,
        phandle_props: Optional[Set[str]] = None,
        max_depth: int = 10
    ) -> List[pyfdt.FdtNode]:
        """
        Recursively follow phandle references from a node.

        Args:
            node: Starting FDT node
            phandle_props: Set of property names to follow (defaults to DEFAULT_PHANDLE_PROPERTIES)
            max_depth: Maximum recursion depth

        Returns:
            List of referenced nodes (deduplicated by path)
        """
        if phandle_props is None:
            phandle_props = set(DEFAULT_PHANDLE_PROPERTIES)

        visited_paths: Set[str] = set()
        referenced_paths: Dict[str, pyfdt.FdtNode] = {}

        def follow_recursive(n: pyfdt.FdtNode, depth: int):
            if depth > max_depth:
                return

            node_path = self.get_node_path(n)
            if node_path in visited_paths:
                return
            visited_paths.add(node_path)

            for sub in n.subdata:
                if not hasattr(sub, 'get_name'):
                    continue
                prop_name = sub.get_name()
                if prop_name not in phandle_props:
                    continue
                if isinstance(sub, pyfdt.FdtNode):
                    continue

                phandles = self.extract_phandles_from_property(n, prop_name)
                for phandle in phandles:
                    ref_node = self.get_node_by_phandle(phandle)
                    if ref_node:
                        ref_path = self.get_node_path(ref_node)
                        if ref_path not in referenced_paths:
                            referenced_paths[ref_path] = ref_node
                            follow_recursive(ref_node, depth + 1)

        follow_recursive(node, 0)
        return list(referenced_paths.values())


def compare_nodes(
    kernel_node: pyfdt.FdtNode,
    reference_node: pyfdt.FdtNode,
    kernel_helper: FDTHelper,
    reference_helper: FDTHelper,
    ignore_props: Optional[Set[str]] = None
) -> List[str]:
    """
    Compare two FDT nodes and return list of differences.

    Args:
        kernel_node: Node from kernel DTS
        reference_node: Node from reference DTS
        kernel_helper: FDTHelper for kernel FDT
        reference_helper: FDTHelper for reference FDT
        ignore_props: Set of property names to ignore

    Returns:
        List of difference descriptions
    """
    differences = []

    if ignore_props is None:
        ignore_props = {'phandle', 'linux,phandle'}

    # Get all properties from reference node
    ref_props = {}
    for sub in reference_node.subdata:
        if hasattr(sub, 'get_name') and not isinstance(sub, pyfdt.FdtNode):
            ref_props[sub.get_name()] = sub

    # Get all properties from kernel node
    kernel_props = {}
    for sub in kernel_node.subdata:
        if hasattr(sub, 'get_name') and not isinstance(sub, pyfdt.FdtNode):
            kernel_props[sub.get_name()] = sub

    # Check for missing properties in kernel
    for name, ref_prop in ref_props.items():
        if name in ignore_props:
            continue
        if name not in kernel_props:
            differences.append(f"Property '{name}' missing in kernel DTS")
        else:
            # Compare values
            kernel_prop = kernel_props[name]
            if not _props_equal(kernel_prop, ref_prop):
                differences.append(f"Property '{name}' differs from reference")

    return differences


def _props_equal(prop1: Any, prop2: Any) -> bool:
    """Compare two FDT properties for equality."""
    if type(prop1) != type(prop2):
        return False

    if isinstance(prop1, pyfdt.FdtPropertyWords):
        return prop1.words == prop2.words
    elif isinstance(prop1, pyfdt.FdtPropertyStrings):
        return prop1.strings == prop2.strings
    elif isinstance(prop1, pyfdt.FdtPropertyBytes):
        return prop1.bytes == prop2.bytes
    elif isinstance(prop1, pyfdt.FdtNop):
        return True

    # For other types, try string comparison
    try:
        return str(prop1) == str(prop2)
    except:
        return False


if __name__ == '__main__':
    # Simple test
    import argparse

    parser = argparse.ArgumentParser(description='DTS Processing Utilities Test')
    parser.add_argument('--dts', required=True, help='Path to DTS file')
    parser.add_argument('--config', help='Path to .config file')
    parser.add_argument('--include-path', '-I', action='append', default=[], help='Include path')

    args = parser.parse_args()

    # Collect include paths
    include_paths = list(args.include_path)
    if args.config:
        include_paths.extend(read_config_include_paths(args.config))
    include_paths.extend(get_default_include_paths(args.dts))

    print(f"DTS file: {args.dts}")
    print(f"Include paths:")
    for p in include_paths:
        print(f"  {p}")

    # Load and parse
    print("\nLoading DTS...")
    try:
        fdt = load_dts(args.dts, include_paths)
        helper = FDTHelper(fdt)

        print(f"\nPhandle index: {len(helper.build_phandle_index())} entries")
        print(f"Path index: {len(helper.build_path_index())} entries")

        print("\nTop-level nodes:")
        root = helper.get_root()
        for sub in root.subdata:
            if isinstance(sub, pyfdt.FdtNode):
                addr = helper.get_reg_address(sub)
                addr_str = f"0x{addr:x}" if addr is not None else "no reg"
                print(f"  /{sub.get_name()} ({addr_str})")

    except DTSProcessingError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
