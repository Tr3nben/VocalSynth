"""Downloads the selected subset of each dataset into raw/<dataset>/ (docs/CORPUS.md, "Download").

Order of attempts per dataset:
 1. raw/<dataset>/SOURCE.json exists and every file in it has its md5 -> nothing to do (CI cache hit).
 2. Range requests: read the archive's central directory and only the selected members (not for
    full_download datasets).
 3. Whole archive: stream it to disk (md5 of the archive checked), read the members locally, delete it.
Every extracted member is checked against its CRC32 (zip) and, where the manifest has one, its md5.
"""

from __future__ import annotations

import json
import os
import re
import time
import urllib.request

from . import remotezip
from .common import log, md5_file, write_json
from .datasets import DATASETS, load_manifest, select_members

ZENODO_API = "https://zenodo.org/api/records/{record}"
ZENODO_FILE = "https://zenodo.org/records/{record}/files/{key}?download=1"


def zenodo_files(record: int) -> list[dict]:
    """[{key, size, md5, url}] of a Zenodo record (InvenioRDM API; the legacy format is accepted too)."""
    req = urllib.request.Request(ZENODO_API.format(record=record), headers={"User-Agent": remotezip.USER_AGENT,
                                                                           "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        data = json.load(resp)
    files = data.get("files", [])
    if isinstance(files, dict):  # some API versions: {"entries": [...]}
        files = files.get("entries", [])
    out = []
    for f in files:
        key = f.get("key") or f.get("filename")
        checksum = f.get("checksum", "")
        links = f.get("links", {})
        url = links.get("content") or links.get("self") or links.get("download") or ZENODO_FILE.format(record=record, key=key)
        out.append({"key": key, "size": f.get("size") or f.get("filesize"),
                    "md5": checksum.split(":", 1)[1] if checksum.startswith("md5:") else None, "url": url})
    return out


def resolve_archive(name: str, manifest: dict) -> dict:
    """{key, size, md5, url} of the dataset's archive; the pinned md5 wins over the API's."""
    ds = DATASETS[name]
    try:
        files = zenodo_files(ds.record)
    except Exception as e:  # noqa: BLE001 - fall back to the plain download URL
        log(f"  Zenodo API not reachable ({e}); using the direct file URL")
        files = []

    pattern = re.compile(ds.zip_pattern)
    candidates = [f for f in files if (ds.zip_name and f["key"] == ds.zip_name) or (not ds.zip_name and pattern.match(f["key"] or ""))]
    fallback_key = ds.zip_name or ds.zip_prefer
    if not candidates and fallback_key and (ds.zip_name or not files):  # try the known file name
        candidates = [{"key": fallback_key, "size": None, "md5": None, "url": ZENODO_FILE.format(record=ds.record, key=fallback_key)}]
    if not candidates:
        raise RuntimeError(f"{name}: no archive in Zenodo record {ds.record} (files: {[f['key'] for f in files]})")

    preferred = [f for f in candidates if ds.zip_prefer and f["key"] == ds.zip_prefer]
    if ds.zip_name:
        archive = candidates[0]
    else:
        # Several archives (VocalSet 1.2: the same recordings in different folder layouts): the preferred
        # one, else the smallest - the subset is chosen by file name, so any layout works, and a whole
        # download (no range support) then costs least.
        archive = preferred[0] if preferred else min(candidates, key=lambda f: f["size"] or float("inf"))
    pinned = manifest.get("zip_md5")
    if pinned and archive["md5"] and pinned != archive["md5"]:
        raise RuntimeError(f"{name}: Zenodo lists md5 {archive['md5']} for {archive['key']}, pinned {pinned}")
    archive["md5"] = pinned or archive["md5"]
    archive["md5_pinned"] = bool(pinned)
    log(f"  archive {archive['key']} ({(archive['size'] or 0) / 1e6:.0f} MB, md5 {archive['md5']}, "
        f"{'pinned' if pinned else 'from the Zenodo API'})")
    return archive


def _cache_ok(dest_dir: str) -> bool:
    source = os.path.join(dest_dir, "SOURCE.json")
    if not os.path.exists(source):
        return False
    with open(source, encoding="utf-8") as f:
        info = json.load(f)
    for rel, md5 in info.get("files", {}).items():
        path = os.path.join(dest_dir, rel)
        if not os.path.exists(path) or md5_file(path) != md5:
            log(f"  cached file missing or changed: {rel}")
            return False
    return bool(info.get("files"))


def _extract(src: remotezip.Source, selection: dict[str, str], members: dict, dest_dir: str, manifest: dict,
             zip_path: str | None) -> dict[str, str]:
    expected = manifest.get("members", {})
    out = {}
    for i, (member, rel) in enumerate(sorted(selection.items(), key=lambda kv: members[kv[0]].header_offset)):
        dest = os.path.join(dest_dir, rel)
        try:
            remotezip.extract_member(src, members[member], dest)
        except remotezip.UnsupportedMember:
            if zip_path is None:
                raise
            remotezip.extract_with_unzip(zip_path, member, dest)
        md5 = md5_file(dest)
        if rel in expected and expected[rel] != md5:
            raise RuntimeError(f"{rel}: md5 {md5}, manifest {expected[rel]}")
        out[rel] = md5
        if (i + 1) % 20 == 0:
            log(f"  {i + 1}/{len(selection)} files")
    return out


def fetch(name: str, raw_dir: str, source: str | None = None, manifest_dir: str | None = None, use_ranges: bool = True,
          vocalset_per_context: int = 1, work_dir: str | None = None) -> dict:
    """Fetches one dataset; source overrides the archive location (local path or URL, md5 from the manifest)."""
    ds = DATASETS[name]
    manifest = load_manifest(name, manifest_dir)
    dest_dir = os.path.join(raw_dir, name)
    log(f"[{name}] {ds.title}")

    if _cache_ok(dest_dir):
        log(f"  cached in {dest_dir}")
        with open(os.path.join(dest_dir, "SOURCE.json"), encoding="utf-8") as f:
            return json.load(f)

    if source:
        archive = {"key": os.path.basename(source.split("?")[0]), "size": None, "md5": manifest.get("zip_md5"),
                   "md5_pinned": bool(manifest.get("zip_md5")), "url": source}
    else:
        archive = resolve_archive(name, manifest)

    started = time.monotonic()
    method = None
    files: dict[str, str] = {}
    src = None

    if use_ranges and not ds.full_download and not os.path.exists(archive["url"]):
        try:
            src = remotezip.Source(archive["url"])
            members = remotezip.list_members(src)
            selection = select_members(name, list(members), manifest, vocalset_per_context)
            log(f"  range requests: {len(selection)} of {len(members)} archive members")
            files = _extract(src, selection, members, dest_dir, manifest, None)
            method = "range"
        except (remotezip.NoRangeSupport, remotezip.UnsupportedMember) as e:
            log(f"  no partial download ({e}); fetching the whole archive")
            files = {}

    if method is None:
        if os.path.exists(archive["url"]):
            zip_path = archive["url"]
            md5 = md5_file(zip_path)
            if archive["md5"] and md5 != archive["md5"]:
                raise RuntimeError(f"{zip_path}: md5 {md5}, expected {archive['md5']}")
            method = "local"
            delete_after = False
        else:
            zip_path = os.path.join(work_dir or raw_dir, f".{name}-{archive['key']}")
            remotezip.download(archive["url"], zip_path, archive["md5"], archive["size"])
            method = "full"
            delete_after = True
        try:
            src = remotezip.Source(zip_path)
            members = remotezip.list_members(src)
            selection = select_members(name, list(members), manifest, vocalset_per_context)
            log(f"  extracting {len(selection)} of {len(members)} archive members")
            files = _extract(src, selection, members, dest_dir, manifest, zip_path)
        finally:
            if delete_after and os.path.exists(zip_path):
                os.remove(zip_path)

    missing = sorted(set(manifest.get("members", {})) - set(files)) if name == "dcs" else []
    if missing:
        log(f"  WARNING: {len(missing)} manifest files not in the archive, e.g. {missing[:3]}")
    if not files:
        raise RuntimeError(f"{name}: no files selected")

    info = {
        "dataset": name,
        "title": ds.title,
        "licence": ds.licence,
        "citation": ds.citation,
        "zenodo_record": ds.record,
        "archive": archive["key"],
        "archive_md5": archive["md5"],
        "archive_md5_pinned": archive.get("md5_pinned", False),
        "method": method,
        "requests": src.requests if src else 0,
        "bytes_read": src.bytes_read if src else 0,
        "seconds": round(time.monotonic() - started, 1),
        "missing": missing,
        "files": dict(sorted(files.items())),
    }
    os.makedirs(dest_dir, exist_ok=True)
    write_json(os.path.join(dest_dir, "SOURCE.json"), info)
    log(f"  {len(files)} files via {method} in {info['seconds']} s ({info['bytes_read'] / 1e6:.1f} MB read)")
    return info
