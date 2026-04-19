#!/usr/bin/env python3
"""
Convert macOS `sample` output to flamegraph collapsed-stack format.

Each output line:  frame0;frame1;...;frameN  COUNT
where COUNT is the number of samples where frameN was on top of the stack
(self/exclusive time), and the semicolon-separated path is the full call chain.

Usage:
    sample <pid> -file out.txt
    python3 sample_to_flamegraph.py out.txt [options]

Options:
    --min-count N      Suppress entries with fewer than N samples (default: 1)
    --app-only         Filter out frames from system libraries
    --no-summary       Skip the summary section printed to stderr
"""

import re
import sys
import argparse
from collections import defaultdict


# ---------------------------------------------------------------------------
# Name cleanup
# ---------------------------------------------------------------------------

def simplify_name(raw: str) -> str:
    """Strip module/offset/address/source annotations; lightly tidy C++ names."""
    s = raw.strip()
    # Remove " (in LibraryName)"
    s = re.sub(r'\s+\(in [^)]+\)', '', s)
    # Remove trailing " + offset  [0xaddr,...]  file.cpp:line"
    s = re.sub(r'\s+\+\s+\d[\d,]*\b.*$', '', s)
    # Clean up ABI tags: [abi:ne210108] etc.
    s = re.sub(r'\[abi:[^\]]+\]', '', s)
    # Collapse extremely long mt19937/uniform_int instantiations
    s = re.sub(
        r'std::mersenne_twister_engine<[^>]+(?:>[^>]+)*>',
        'std::mt19937_64', s)
    s = re.sub(
        r'(std::uniform_int_distribution<[^>]+>::operator\(\)<)std::mt19937_64',
        r'\1std::mt19937_64', s)
    # Collapse repeated comma-separated addresses in brackets (already stripped above)
    return s.strip()


def is_system_frame(name: str) -> bool:
    system_prefixes = (
        'start ', 'dyld_start', '_dyld_', 'dyld::', '_pthread_', 'thread_start',
        '__ulock_wait', '__semwait_signal', 'nanosleep', '_tlv_get_addr',
        '_platform_memset', 'DYLD-STUB$$',
    )
    return any(name.startswith(p) for p in system_prefixes)


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def parse_call_graph(lines: list[str]) -> list[tuple[int, int, str]]:
    """
    Parse the "Call graph:" section into a flat list of (depth, count, name).

    Depth is derived from the length of the branch-symbol prefix on each line
    (the sequence of '+', '!', ':', '|', ' ' characters before the count).
    Each two-character unit corresponds to one depth level:
      depth 0 → thread line (no prefix beyond the base 4-space indent)
      depth 1 → "+ "   (2 chars)
      depth 2 → "+   " (4 chars)
      depth N → 2*N chars of prefix
    """
    nodes = []
    for raw in lines:
        if not raw.startswith('    '):
            continue
        rest = raw[4:]            # strip base 4-space indent common to all graph lines
        if not rest:
            continue

        m = re.match(r'^([\+\!\:\| ]*?)(\d[\d,]*)\s+(.+)$', rest)
        if not m:
            continue

        prefix   = m.group(1)
        count    = int(m.group(2).replace(',', ''))
        raw_name = m.group(3)

        depth = len(prefix) // 2
        name  = simplify_name(raw_name)
        if name:
            nodes.append((depth, count, name))

    return nodes


def build_collapsed(nodes: list[tuple[int, int, str]]) -> dict[tuple, int]:
    """
    Walk the parsed node list with a stack and compute self (exclusive) counts.

    For every node:  self_count = node.count - sum(direct_children.count)
    Returns a mapping  stack_path_tuple → self_count.
    """
    collapsed: dict[tuple, int] = defaultdict(int)

    # Stack entries: (depth, name, inclusive_count, children_sum_so_far)
    stack: list[tuple[int, str, int, int]] = []

    def flush_node(depth, name, count, children_sum):
        path   = tuple(s[1] for s in stack) + (name,)
        self_c = count - children_sum
        if self_c > 0:
            collapsed[path] += self_c

    def pop_until(target_depth):
        """Pop all nodes at depth >= target_depth, propagating counts upward."""
        while stack and stack[-1][0] >= target_depth:
            pd, pn, pc, pch = stack.pop()
            flush_node(pd, pn, pc, pch)
            # Credit this node's count to its parent's children_sum
            if stack and stack[-1][0] == pd - 1:
                gd, gn, gc, gch = stack[-1]
                stack[-1] = (gd, gn, gc, gch + pc)

    for depth, count, name in nodes:
        pop_until(depth)
        stack.append((depth, name, count, 0))

    pop_until(-1)   # flush everything remaining
    return collapsed


# ---------------------------------------------------------------------------
# Formatting helpers
# ---------------------------------------------------------------------------

_SHORTEN = [
    # Lambda/closure suffixes — keep outer class, drop lambda body
    (re.compile(r'::\$_\d+::operator\(\)\(\) const$'), '::lambda()'),
    (re.compile(r'::\$_\d+::operator\(\)\(.+\) const$'), '::lambda(...)'),
    (re.compile(r"::'lambda'\(void\*, PageRef\)::__invoke\(void\*, PageRef\)$"),
     '::FunctionRef::invoke'),
    # std boilerplate
    (re.compile(r'^std::__thread_proxy<.+>$'), 'std::__thread_proxy<...>'),
    (re.compile(r'^std::this_thread::sleep_for<.+>$'), 'std::this_thread::sleep_for'),
]

def short_name(name: str) -> str:
    """Shorten a cleaned C++ name for display."""
    for pat, repl in _SHORTEN:
        if pat.search(name):
            name = pat.sub(repl, name)
            break
    # Cap at 90 chars
    if len(name) > 90:
        name = name[:87] + '...'
    return name


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description='Convert macOS `sample` output to flamegraph collapsed format.')
    ap.add_argument('sample_file', help='Path to the `sample` output file.')
    ap.add_argument('--min-count', type=int, default=1,
                    help='Minimum self-sample count to emit (default: 1).')
    ap.add_argument('--app-only', action='store_true',
                    help='Suppress frames from system libraries.')
    ap.add_argument('--no-summary', action='store_true',
                    help='Skip the human-readable summary on stderr.')
    args = ap.parse_args()

    with open(args.sample_file) as f:
        raw_lines = f.readlines()

    # Locate the "Call graph:" section
    start_idx = end_idx = None
    for i, line in enumerate(raw_lines):
        if line.strip() == 'Call graph:':
            start_idx = i + 1
        elif start_idx and line.strip().startswith('Sort by top of stack'):
            end_idx = i
            break
    if start_idx is None:
        sys.exit('ERROR: "Call graph:" section not found in input.')
    graph_lines = [l.rstrip() for l in raw_lines[start_idx:end_idx]]

    nodes    = parse_call_graph(graph_lines)
    collapsed = build_collapsed(nodes)

    # Filter and sort
    entries = sorted(
        ((path, cnt) for path, cnt in collapsed.items() if cnt >= args.min_count),
        key=lambda x: -x[1]
    )

    # Emit flamegraph collapsed format to stdout
    for path, cnt in entries:
        frames = path
        if args.app_only:
            frames = tuple(f for f in frames if not is_system_frame(f))
        if not frames:
            continue
        print(f"{';'.join(frames)} {cnt}")

    # Summary to stderr
    if not args.no_summary:
        total = sum(cnt for _, cnt in entries)
        print(f"\n{'='*70}", file=sys.stderr)
        print(f"  Total on-CPU samples:  {total:,}", file=sys.stderr)
        print(f"  Unique stack paths:    {len(entries):,}", file=sys.stderr)
        print(f"\n  Top 20 hottest leaf frames (self time):", file=sys.stderr)
        print(f"  {'Samples':>8}  {'%':>6}  Frame", file=sys.stderr)
        print(f"  {'-'*8}  {'-'*6}  {'-'*55}", file=sys.stderr)

        # Aggregate by leaf frame name
        by_leaf: dict[str, int] = defaultdict(int)
        for path, cnt in entries:
            by_leaf[path[-1]] += cnt

        for name, cnt in sorted(by_leaf.items(), key=lambda x: -x[1])[:20]:
            pct = 100 * cnt / total if total else 0
            print(f"  {cnt:>8,}  {pct:>5.1f}%  {short_name(name)}", file=sys.stderr)

        print(f"\n  Top 20 hottest call chains:", file=sys.stderr)
        print(f"  {'Samples':>8}  {'%':>6}  Chain", file=sys.stderr)
        print(f"  {'-'*8}  {'-'*6}  {'-'*55}", file=sys.stderr)
        for path, cnt in entries[:20]:
            pct = 100 * cnt / total if total else 0
            chain = ' → '.join(short_name(f) for f in path[-4:])
            if len(path) > 4:
                chain = '... → ' + chain
            print(f"  {cnt:>8,}  {pct:>5.1f}%  {chain}", file=sys.stderr)
        print(file=sys.stderr)


if __name__ == '__main__':
    main()
