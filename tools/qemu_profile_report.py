#!/usr/bin/env python3
"""
qemu_profile_report.py — post-process QEMU hotblocks/hotpages plugin output.

Reads the combined plugin log produced by a `-plugin libhotblocks -plugin
libhotpages -d plugin -D <log>` run, splits it into the two plugins' sections,
and emits human-readable top-N summaries:

  * hotblocks: sorted by ecount (execution count) descending, each basic-block
    PC annotated with the nearest preceding kernel symbol (from `nm` over the
    unstripped Kernel.debug). PCs are guest virtual addresses.
  * hotpages:  sorted by reads+writes descending. Addresses are guest *physical*
    pages, so they are not symbol-annotated.

Raw per-plugin sections are written to <out>/hotblocks.txt and <out>/hotpages.txt;
summaries to <out>/hotblocks_summary.txt and <out>/hotpages_summary.txt; both
summaries are also echoed to stdout.

Plugin output formats (QEMU 9.2.1, contrib/plugins):
  hotblocks:  "pc, tcount, icount, ecount"   rows "0x%016x, %d, %ld, %ld"
  hotpages:   "Addr, RCPUs, Reads, WCPUs, Writes"
              rows "0x%016x, 0x%04x, %ld, 0x%04x, %ld"
"""

import argparse
import bisect
import subprocess
import sys
from pathlib import Path

HOTBLOCKS_HDR = "pc, tcount, icount, ecount"
HOTPAGES_HDR = "Addr, RCPUs, Reads, WCPUs, Writes"

def caveats(note):
    base = (
        "# Caveats:\n"
        "#  - Full-system TCG: basic-block counts are noisy — blocks get\n"
        "#    re-translated as mappings change, so a single hot routine may appear\n"
        "#    as several block entries.\n"
        "#  - Single-threaded TCG only (-accel tcg,thread=single); plugin counters\n"
        "#    are not multi-thread-safe.\n"
    )
    if note:
        base += "#  - " + note.replace("\n", "\n#    ") + "\n"
    return base


def split_sections(log_path):
    """Return (hotblocks_lines, hotpages_lines) raw sections from the combined log."""
    blocks, pages = [], []
    target = None
    for raw in Path(log_path).read_text(errors="replace").splitlines():
        line = raw.rstrip("\n")
        if line.startswith("collected ") and "entries in the hash table" in line:
            target = blocks
            blocks.append(line)
            continue
        if line == HOTBLOCKS_HDR:
            target = blocks
            blocks.append(line)
            continue
        if line == HOTPAGES_HDR:
            target = pages
            pages.append(line)
            continue
        if target is not None:
            target.append(line)
    return blocks, pages


def demangle(names, cxxfilt):
    """Batch-demangle C++ symbol names; return unchanged on any failure."""
    if not names:
        return names
    try:
        out = subprocess.run(
            [cxxfilt], input="\n".join(names), capture_output=True, text=True, check=True
        ).stdout.splitlines()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return names
    return out if len(out) == len(names) else names


def load_symbols(nm_tool, kernel_debug, cxxfilt):
    """Return sorted (addrs, names) of code symbols from `nm -n` over kernel_debug."""
    addrs, names = [], []
    try:
        out = subprocess.run(
            [nm_tool, "-n", kernel_debug],
            capture_output=True, text=True, check=True,
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print(f"warning: symbol resolution disabled ({e})", file=sys.stderr)
        return addrs, names
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        addr_s, typ, name = parts[0], parts[1], parts[2]
        if typ not in ("t", "T", "w", "W"):  # code symbols only
            continue
        try:
            addr = int(addr_s, 16)
        except ValueError:
            continue
        addrs.append(addr)
        names.append(name)
    return addrs, demangle(names, cxxfilt)


def symbolize(pc, addrs, names):
    if not addrs:
        return "?"
    i = bisect.bisect_right(addrs, pc) - 1
    if i < 0:
        return "?"
    off = pc - addrs[i]
    return f"{names[i]}+0x{off:x}" if off else names[i]


def parse_hotblocks(lines):
    rows = []
    for line in lines:
        if not line.startswith("0x"):
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) != 4:
            continue
        try:
            pc = int(cols[0], 16)
            tcount = int(cols[1])
            icount = int(cols[2])
            ecount = int(cols[3])
        except ValueError:
            continue
        rows.append((pc, tcount, icount, ecount))
    rows.sort(key=lambda r: r[3], reverse=True)
    return rows


def parse_hotpages(lines):
    rows = []
    for line in lines:
        if not line.startswith("0x"):
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) != 5:
            continue
        try:
            addr = int(cols[0], 16)
            reads = int(cols[2])
            writes = int(cols[4])
        except ValueError:
            continue
        rows.append((addr, reads, writes))
    rows.sort(key=lambda r: r[1] + r[2], reverse=True)
    return rows


def format_hotblocks(rows, top, addrs, names, note):
    out = []
    out.append("=== QEMU hotblocks — top {} basic blocks by execution count ===".format(top))
    out.append(caveats(note))
    out.append(f"{'ecount':>16}  {'icount':>8}  {'tcount':>6}  {'pc':>18}  symbol")
    out.append("-" * 80)
    for pc, tcount, icount, ecount in rows[:top]:
        out.append(f"{ecount:>16}  {icount:>8}  {tcount:>6}  0x{pc:016x}  {symbolize(pc, addrs, names)}")
    if not rows:
        out.append("(no hotblocks data — was libhotblocks attached?)")
    return "\n".join(out) + "\n"


def format_hotpages(rows, top, note):
    out = []
    out.append("=== QEMU hotpages — top {} guest-physical pages by total accesses ===".format(top))
    out.append(caveats(note))
    out.append(f"{'reads+writes':>14}  {'reads':>12}  {'writes':>12}  {'phys page':>18}")
    out.append("-" * 64)
    for addr, reads, writes in rows[:top]:
        out.append(f"{reads + writes:>14}  {reads:>12}  {writes:>12}  0x{addr:016x}")
    if not rows:
        out.append("(no hotpages data — was libhotpages attached?)")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--log", required=True, help="combined plugin -D log")
    ap.add_argument("--kernel-debug", required=True, help="unstripped Kernel.debug for nm")
    ap.add_argument("--nm", default="x86_64-elf-nm", help="nm tool")
    ap.add_argument("--cxxfilt", default="x86_64-elf-c++filt", help="c++filt tool for demangling")
    ap.add_argument("--out-dir", required=True, help="output directory")
    ap.add_argument("--top", type=int, default=20, help="entries per summary")
    ap.add_argument("--note", default="", help="extra caveat line (e.g. ROI status)")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    blocks_lines, pages_lines = split_sections(args.log)
    (out_dir / "hotblocks.txt").write_text("\n".join(blocks_lines) + "\n")
    (out_dir / "hotpages.txt").write_text("\n".join(pages_lines) + "\n")

    addrs, names = load_symbols(args.nm, args.kernel_debug, args.cxxfilt)

    blocks_summary = format_hotblocks(parse_hotblocks(blocks_lines), args.top, addrs, names, args.note)
    pages_summary = format_hotpages(parse_hotpages(pages_lines), args.top, args.note)

    (out_dir / "hotblocks_summary.txt").write_text(blocks_summary)
    (out_dir / "hotpages_summary.txt").write_text(pages_summary)

    print(blocks_summary)
    print(pages_summary)


if __name__ == "__main__":
    main()
