#!/usr/bin/env python3
"""Publish public web pages as static Microbrowser Reader documents."""

from __future__ import annotations

import argparse
from collections.abc import Sequence
import json
import os
from pathlib import Path
import re
import urllib.parse
import sys

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from gateway.extract import URLPolicy, extract_html, fetch

SLUG = re.compile(r"[a-z][a-z0-9-]{1,63}")


def _source_slug(value: str) -> str:
    slug, separator, url = value.partition("=")
    if not separator or not SLUG.fullmatch(slug):
        raise ValueError("sources must have the form lowercase-slug=https://example.com/page")
    return slug, url


def _image_source(block: dict[str, object], document_url: str) -> str | None:
    candidate = block.get("source_url", block.get("url"))
    if not isinstance(candidate, str):
        return None
    if candidate.startswith("/v1/asset?"):
        source = urllib.parse.parse_qs(urllib.parse.urlsplit(candidate).query).get("url", [])
        candidate = source[0] if len(source) == 1 else None
    return urllib.parse.urljoin(document_url, candidate) if candidate else None


def publish_source(slug: str, url: str, allow_private_hosts: bool) -> dict[str, object]:
    """Convert one currently public HTML page, or raise an explicit conversion error."""
    policy = URLPolicy(allow_private_hosts=allow_private_hosts)
    resource = fetch(url, policy)
    document = extract_html(resource.decode_html(), resource.url)
    if document["kind"] == "unsupported" or not document["blocks"]:
        raise ValueError(f"{slug}: the page requires interactive mode and cannot be published as static Reader content")
    if document["mode"] != "reader":
        raise ValueError("static snapshots cannot contain interactive sessions")
    for block in document["blocks"]:
        if not isinstance(block, dict) or block.get("type") != "image":
            continue
        image_source = _image_source(block, resource.url)
        if image_source is None:
            raise ValueError(f"{slug}: an image has no usable HTTP(S) source")
        policy.validate(image_source)
        block["url"] = image_source
        block.pop("source_url", None)
    document["interactive_available"] = False
    document["session"] = None
    return document


def write_json(path: Path, value: dict[str, object]) -> None:
    """Atomically replace one UTF-8 Reader artifact."""
    temporary = path.with_suffix(path.suffix + ".tmp")
    try:
        temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def publish_sources(
    sources: Sequence[tuple[str, str]],
    destination: Path,
    site_base: str | None = None,
    allow_private_hosts: bool = False,
) -> list[Path]:
    """Write independent documents and a linked static index."""
    destination.mkdir(parents=True, exist_ok=True)
    published: list[Path] = []
    seen: set[str] = set()
    links = []
    for slug, url in sources:
        if not SLUG.fullmatch(slug) or slug in seen:
            raise ValueError("each source needs a unique lowercase-slug")
        seen.add(slug)
        document = publish_source(slug, url, allow_private_hosts)
        filename = slug + ".mbr.json"
        path = destination / filename
        write_json(path, document)
        published.append(path)
        target = urllib.parse.urljoin(site_base, filename) if site_base else filename
        links.append({"type": "link", "text": document["title"], "url": target})
    index = {
        "format": "microbrowser-reader-v1",
        "kind": "article", "mode": "reader",
        "title": "Microbrowser reading list",
        "source_url": site_base + "index.mbr.json" if site_base else "index.mbr.json",
        "previous_url": None, "next_url": None,
        "interactive_available": False, "session": None,
        "blocks": [
            {"type": "paragraph", "text": "Static Reader snapshots. Links to other websites may use the live gateway."},
            *links,
        ],
    }
    index_path = destination / "index.mbr.json"
    write_json(index_path, index)
    published.append(index_path)
    return published


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", nargs="+", metavar="SLUG=URL", help="public pages to convert")
    parser.add_argument("--output", type=Path, required=True, help="static-host publishing directory")
    parser.add_argument("--site-base", help="published HTTPS directory, used for index links")
    parser.add_argument("--allow-private-hosts", action="store_true", help="development fixtures only; not for public deployment")
    arguments = parser.parse_args()
    sources = [_source_slug(value) for value in arguments.sources]
    published = publish_sources(sources, arguments.output, arguments.site_base, arguments.allow_private_hosts)
    print(f"Published {len(sources)} Reader documents and index.mbr.json in {arguments.output}")
    if any(path.parent != arguments.output for path in published):
        raise ValueError("published artifact escaped its destination")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
