#!/usr/bin/env python3
"""Render an ares N64 profiler folded-stack capture as interactive HTML."""

from __future__ import annotations

import argparse
import hashlib
import html
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Node:
    name: str
    value: int = 0
    children: dict[str, "Node"] = field(default_factory=dict)


def read_folded(path: Path) -> Node:
    root = Node("all")
    for line_number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip():
            continue
        try:
            stack, value_text = line.rsplit(" ", 1)
            value = int(value_text)
        except ValueError as error:
            raise SystemExit(f"{path}:{line_number}: invalid folded stack") from error
        if value <= 0:
            continue
        root.value += value
        node = root
        for name in stack.split(";"):
            node = node.children.setdefault(name, Node(name))
            node.value += value
    if not root.value:
        raise SystemExit(f"{path}: capture contains no samples")
    return root


def color(name: str) -> str:
    digest = hashlib.sha1(name.encode()).digest()
    return f"rgb(240,{90 + digest[0] % 100},{50 + digest[1] % 55})"


def render(input_path: Path, output_path: Path, width: int) -> None:
    root = read_folded(input_path)
    frame_height = 22

    max_depth = 0
    pending_depth = [(root, 1)]
    while pending_depth:
        node, node_depth = pending_depth.pop()
        max_depth = max(max_depth, node_depth)
        pending_depth.extend((child, node_depth + 1) for child in node.children.values())

    height = 16 + frame_height * (max_depth - 1)
    elements: list[str] = []

    pending_nodes = [(root, 0.0, 0, 1.0)]
    while pending_nodes:
        node, x, level, node_width = pending_nodes.pop()
        if level:
            percent = 100.0 * node.value / root.value
            name = html.escape(node.name, quote=True)
            elements.append(
                f'<div class="frame" data-name="{name}" data-cycles="{node.value}" '
                f'data-percent="{percent:.3f}" style="left:{x * 100:.8f}%;'
                f'width:{node_width * 100:.8f}%;bottom:{(level - 1) * frame_height}px;'
                f'background:{color(node.name)}"><span>{name}</span></div>'
            )
        children = sorted(node.children.values(), key=lambda item: (-item.value, item.name))
        child_x = x
        positioned_children = []
        for child in children:
            child_width = node_width * child.value / node.value
            positioned_children.append((child, child_x, level + 1, child_width))
            child_x += child_width
        pending_nodes.extend(reversed(positioned_children))

    title = html.escape(f"ares N64 CPU flame graph — {input_path.name}")
    document = f'''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
  * {{ box-sizing: border-box; }}
  body {{ margin: 0; color: #222; background: #fafafa; font: 13px system-ui, sans-serif; }}
  header {{ position: sticky; left: 0; padding: 10px 12px; background: #fff; border-bottom: 1px solid #ccc; z-index: 3; }}
  h1 {{ display: inline; margin: 0 14px 0 0; font-size: 15px; }}
  .hint {{ color: #666; }}
  #viewport {{ width: 100vw; overflow-x: auto; overflow-y: hidden; padding: 10px; }}
  #graph {{ position: relative; width: {width}px; height: {height}px; min-width: {width}px; transform-origin: left center; }}
  .frame {{ position: absolute; height: {frame_height - 1}px; overflow: hidden; border: 1px solid rgba(120,40,20,.45); border-radius: 2px; white-space: nowrap; cursor: default; }}
  .frame:hover {{ filter: brightness(1.12); border-color: #111; z-index: 2; }}
  .frame span {{ display: block; padding: 2px 4px; overflow: hidden; font: 11px/15px ui-monospace, monospace; pointer-events: none; }}
  #tooltip {{ position: fixed; display: none; max-width: min(680px, 90vw); padding: 7px 9px; color: #fff; background: rgba(20,20,20,.94); border-radius: 4px; pointer-events: none; z-index: 10; font: 12px ui-monospace, monospace; overflow-wrap: anywhere; }}
</style>
</head>
<body>
<header><h1>{title}</h1><span class="hint">Wheel up/down to zoom width · Shift+wheel or the scrollbar to pan</span></header>
<main id="viewport"><div id="graph">{''.join(elements)}</div></main>
<div id="tooltip"></div>
<script>
(() => {{
  const viewport = document.getElementById('viewport');
  const graph = document.getElementById('graph');
  const tooltip = document.getElementById('tooltip');
  const baseWidth = {width};
  let scale = 1;

  viewport.addEventListener('wheel', event => {{
    if (event.shiftKey) return;
    event.preventDefault();
    const rect = viewport.getBoundingClientRect();
    const pointer = event.clientX - rect.left;
    const graphPoint = viewport.scrollLeft + pointer;
    const ratio = graphPoint / graph.offsetWidth;
    scale = Math.min(64, Math.max(1, scale * Math.exp(-event.deltaY * 0.002)));
    graph.style.width = `${{Math.round(baseWidth * scale)}}px`;
    requestAnimationFrame(() => {{ viewport.scrollLeft = ratio * graph.offsetWidth - pointer; }});
  }}, {{passive: false}});

  graph.addEventListener('pointermove', event => {{
    const frame = event.target.closest('.frame');
    if (!frame) {{ tooltip.style.display = 'none'; return; }}
    tooltip.textContent = `${{frame.dataset.name}} · ${{Number(frame.dataset.cycles).toLocaleString()}} cycles (${{frame.dataset.percent}}%)`;
    tooltip.style.display = 'block';
    const x = Math.min(event.clientX + 14, innerWidth - tooltip.offsetWidth - 8);
    const y = Math.min(event.clientY + 16, innerHeight - tooltip.offsetHeight - 8);
    tooltip.style.left = `${{Math.max(8, x)}}px`;
    tooltip.style.top = `${{Math.max(8, y)}}px`;
  }});
  graph.addEventListener('pointerleave', () => {{ tooltip.style.display = 'none'; }});
}})();
</script>
</body>
</html>
'''
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(document)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="ares .folded capture")
    parser.add_argument("output", nargs="?", type=Path, help="output HTML")
    parser.add_argument("--width", type=int, default=1400)
    args = parser.parse_args()
    output = args.output or args.input.with_suffix(".html")
    render(args.input, output, args.width)
    print(output)


if __name__ == "__main__":
    main()
