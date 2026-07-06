#!/usr/bin/env python3
"""Generate the Guide knowledge-graph manifest for the Amber website.

The Guide is a set of bilingual Markdown nodes under
``site/guide/content/<NN-category>/<id>.<lang>.md``. Each file carries a small
YAML-subset frontmatter block (see ``docs/engineering/doc-system.md``). This
tool scans those files, validates the graph, and emits
``site/guide/guide-manifest.json`` which ``site/guide-page.js`` consumes to
build the sidebar, the prerequisite/related rail, and prev/next navigation.

Usage::

    python3 tools/gen_guide_manifest.py            # write the manifest
    python3 tools/gen_guide_manifest.py --check     # validate only, non-zero on error

No third-party dependencies: the frontmatter parser is a purpose-built subset
(scalars, inline ``[a, b]`` lists, and a ``spec_refs`` list of ``anchor``/
``label`` maps), mirroring the hand-rolled YAML helpers in ``spec_sync.py``.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTENT_DIR = ROOT / "site" / "guide" / "content"
CATEGORIES_FILE = CONTENT_DIR / "categories.yaml"
OUTPUT_FILE = ROOT / "site" / "guide" / "guide-manifest.json"
SITE_ROOT = ROOT / "site"

LANGS = ("ru", "en")
# Fields expected to be identical across a node's language files. Titles and
# summaries are per-language and handled separately.
SHARED_FIELDS = ("category", "order", "prerequisites", "related", "status", "spec_refs")


def strip_scalar(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    return value


def parse_inline_list(value: str) -> list[str]:
    inner = value.strip()[1:-1].strip()
    if not inner:
        return []
    return [strip_scalar(item) for item in inner.split(",") if item.strip()]


def parse_frontmatter(text: str, source: Path) -> tuple[dict, str]:
    """Return (frontmatter dict, body). Raises ValueError on malformed input."""
    lines = text.replace("\r\n", "\n").split("\n")
    if not lines or lines[0].strip() != "---":
        raise ValueError(f"{source}: missing opening '---' frontmatter fence")
    end = None
    for i in range(1, len(lines)):
        if lines[i].strip() == "---":
            end = i
            break
    if end is None:
        raise ValueError(f"{source}: missing closing '---' frontmatter fence")

    data: dict = {}
    i = 1
    while i < end:
        raw = lines[i]
        if not raw.strip():
            i += 1
            continue
        match = re.match(r"^([A-Za-z_][\w.]*):\s?(.*)$", raw)
        if not match:
            raise ValueError(f"{source}: cannot parse frontmatter line: {raw!r}")
        key, value = match.group(1), match.group(2)

        if key == "spec_refs" and value.strip() == "":
            refs: list[dict] = []
            i += 1
            current: dict | None = None
            while i < end and (lines[i].startswith(" ") or lines[i].startswith("\t")):
                item = lines[i].strip()
                item_match = re.match(r"^-?\s*([A-Za-z_]\w*):\s?(.*)$", item)
                if not item_match:
                    raise ValueError(f"{source}: bad spec_refs entry: {lines[i]!r}")
                sub_key, sub_val = item_match.group(1), strip_scalar(item_match.group(2))
                if item.startswith("-"):
                    current = {}
                    refs.append(current)
                if current is None:
                    raise ValueError(f"{source}: spec_refs field before '-' item")
                current[sub_key] = sub_val
                i += 1
            data["spec_refs"] = refs
            continue

        if value.strip().startswith("["):
            data[key] = parse_inline_list(value)
        else:
            data[key] = strip_scalar(value)
        i += 1

    body = "\n".join(lines[end + 1 :]).strip("\n")
    return data, body


def parse_categories(errors: list[str]) -> list[dict]:
    """Parse the flat categories.yaml (a list of id/order/title.ru/title.en)."""
    if not CATEGORIES_FILE.exists():
        errors.append(f"missing {CATEGORIES_FILE.relative_to(ROOT)}")
        return []
    categories: list[dict] = []
    current: dict | None = None
    for raw in CATEGORIES_FILE.read_text(encoding="utf-8").split("\n"):
        line = raw.split("#", 1)[0].rstrip() if not raw.strip().startswith("#") else ""
        if not line.strip():
            continue
        match = re.match(r"^\s*-?\s*([A-Za-z_][\w.]*):\s?(.*)$", line)
        if not match:
            continue
        key, value = match.group(1), strip_scalar(match.group(2))
        if line.lstrip().startswith("- ") or key == "id":
            if key == "id":
                current = {"id": value, "title": {}}
                categories.append(current)
                continue
        if current is None:
            continue
        if key == "order":
            current["order"] = int(value)
        elif key.startswith("title."):
            current["title"][key.split(".", 1)[1]] = value
    return categories


def collect_nodes(errors: list[str], warnings: list[str]) -> list[dict]:
    files = sorted(CONTENT_DIR.rglob("*.md"))
    by_id: dict[str, dict] = {}

    for path in files:
        stem = path.name
        lang_match = re.match(r"^(.+)\.(ru|en)\.md$", stem)
        if not lang_match:
            warnings.append(f"skipping non-localized file {path.relative_to(ROOT)}")
            continue
        lang = lang_match.group(2)
        try:
            front, _ = parse_frontmatter(path.read_text(encoding="utf-8"), path)
        except ValueError as exc:
            errors.append(str(exc))
            continue

        node_id = front.get("id")
        if not node_id:
            errors.append(f"{path.relative_to(ROOT)}: missing 'id'")
            continue

        rel = path.relative_to(SITE_ROOT).as_posix()
        entry = by_id.setdefault(node_id, {"id": node_id, "langs": {}})
        entry["langs"][lang] = {"front": front, "path": rel}

    nodes: list[dict] = []
    for node_id, entry in by_id.items():
        langs = entry["langs"]
        present = [l for l in LANGS if l in langs]
        if not present:
            continue
        primary = langs.get("ru") or langs[present[0]]
        front = primary["front"]

        for lang in LANGS:
            if lang not in langs:
                warnings.append(f"node '{node_id}' has no {lang} translation")

        # Shared fields must agree across languages; warn on divergence.
        for lang in present:
            for field in SHARED_FIELDS:
                a = langs[lang]["front"].get(field)
                b = front.get(field)
                if a != b:
                    warnings.append(
                        f"node '{node_id}' field '{field}' differs between "
                        f"languages ({lang}={a!r} vs {b!r})"
                    )

        node = {
            "id": node_id,
            "category": front.get("category", ""),
            "order": int(front.get("order", 0)),
            "status": front.get("status", "draft"),
            "prerequisites": list(front.get("prerequisites", [])),
            "related": list(front.get("related", [])),
            "spec_refs": list(front.get("spec_refs", [])),
            "titles": {l: langs[l]["front"].get("title", node_id) for l in present},
            "summaries": {l: langs[l]["front"].get("summary", "") for l in present},
            "paths": {l: langs[l]["path"] for l in present},
        }
        nodes.append(node)

    nodes.sort(key=lambda n: (n["order"], n["id"]))
    return nodes


def build_edges(nodes: list[dict]) -> list[dict]:
    edges: list[dict] = []
    for node in nodes:
        for target in node["prerequisites"]:
            edges.append({"from": target, "to": node["id"], "kind": "prereq"})
        for target in node["related"]:
            edges.append({"from": node["id"], "to": target, "kind": "related"})
    return edges


def validate(nodes: list[dict], categories: list[dict], errors: list[str]) -> None:
    ids = {n["id"] for n in nodes}
    category_ids = {c["id"] for c in categories}
    for node in nodes:
        if node["category"] not in category_ids:
            errors.append(
                f"node '{node['id']}' references unknown category "
                f"'{node['category']}' (add it to categories.yaml)"
            )
        for target in node["prerequisites"] + node["related"]:
            if target not in ids:
                errors.append(
                    f"node '{node['id']}' links to unknown node '{target}'"
                )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="validate the graph without writing the manifest",
    )
    args = parser.parse_args()

    errors: list[str] = []
    warnings: list[str] = []

    categories = parse_categories(errors)
    categories.sort(key=lambda c: c.get("order", 0))
    nodes = collect_nodes(errors, warnings)
    validate(nodes, categories, errors)

    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)

    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1

    manifest = {
        "schema": "amber.guide.manifest.v1",
        "show_spec_refs": True,
        "categories": categories,
        "nodes": nodes,
        "edges": build_edges(nodes),
    }

    if args.check:
        print(
            f"guide manifest OK: {len(nodes)} nodes, {len(categories)} categories"
        )
        return 0

    OUTPUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_FILE.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(
        f"wrote {OUTPUT_FILE.relative_to(ROOT)}: "
        f"{len(nodes)} nodes, {len(categories)} categories"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
