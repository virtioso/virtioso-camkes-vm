# TODO

## Remove absolute and personal pathnames

## Support and use config fragments

## Tegra234 Device Tree Extraction Tooling

Requested plan: finalize tooling to extract required nodes for seL4 use from Tegra234 device tree sources, including relevant overlay blobs, and produce validated sel4/camkes-ready node selections.

Current unfinished files:
- `tools/dtb-node-generator.py`
- `tools/dts_utils.py`
- `tools/guest-dtb-preview.py`

Expected end state:
- Deterministic extraction flow from base DTS + overlays
- Clear handling of phandle dependencies and exclusions
- Output suitable for `dtb()` entries in CAmkES configs
- Documented invocation examples for Orin AGX workflows
