#!/usr/bin/env python3
"""VocalSynth corpus tool (docs/CORPUS.md).

  corpus.py download --raw DIR [--datasets vocadito,dcs,vocalset] [--source NAME=PATH_OR_URL ...]
                     [--manifest-dir DIR] [--no-ranges] [--vocalset-per-context N]
  corpus.py convert  --raw DIR --out DIR [--datasets ...] [--rate 44100] [--level-db -20]
                     [--vocadito-notes a1|a2|f0] [--jobs N] [--limit N] [--manifest-dir DIR]
  corpus.py fake     --out DIR          fake archives + manifests for dry runs and tests
  corpus.py selfcheck [--work DIR] [--vs-eval PATH]
                     fake -> download -> convert (-> vs-eval) with checks, for machines without pytest

Runs with `python -I` too (it adds only its own directory to sys.path).
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from vscorpus import convert as convert_mod  # noqa: E402
from vscorpus import fake, fetch  # noqa: E402
from vscorpus.common import log  # noqa: E402
from vscorpus.datasets import DATASETS  # noqa: E402


def _datasets(text: str) -> list[str]:
    names = [d.strip() for d in text.split(",") if d.strip()]
    for n in names:
        if n not in DATASETS:
            raise SystemExit(f"unknown dataset {n} (known: {', '.join(DATASETS)})")
    return names


def cmd_download(args) -> int:
    sources = {}
    for s in args.source or []:
        name, _, location = s.partition("=")
        sources[name] = location
    failed = []
    for name in _datasets(args.datasets):
        try:
            fetch.fetch(name, args.raw, sources.get(name), args.manifest_dir, not args.no_ranges,
                        args.vocalset_per_context, args.work)
        except Exception as e:  # noqa: BLE001 - one dataset failing must not stop the others
            log(f"[{name}] FAILED: {type(e).__name__}: {e}")
            failed.append(name)
    if failed:
        log(f"download failed for: {', '.join(failed)}")
    return 1 if failed and (args.strict or len(failed) == len(_datasets(args.datasets))) else 0


def cmd_convert(args) -> int:
    entries = convert_mod.convert(args.raw, args.out, _datasets(args.datasets), args.rate, args.level_db,
                                  args.vocadito_notes, args.jobs, args.limit, args.manifest_dir)
    return 0 if entries else 1


def cmd_fake(args) -> int:
    paths = fake.make_fake_sources(args.out)
    print(json.dumps(paths, indent=1))
    return 0


def cmd_selfcheck(args) -> int:
    work = args.work or tempfile.mkdtemp(prefix="vs-corpus-selfcheck-")
    src = os.path.join(work, "sources")
    raw = os.path.join(work, "raw")
    out = os.path.join(work, "eval")
    paths = fake.make_fake_sources(src)
    for name, path in paths.items():
        fetch.fetch(name, raw, path, os.path.join(src, "manifests"))
    entries = convert_mod.convert(raw, out, list(paths), 44100, -20.0, "a1", 1, 0, os.path.join(src, "manifests"))
    problems = []
    names = {e["name"] for e in entries}
    expect = {"tenor_vocadito_01", "soprano_vocadito_02"}
    if not expect <= names:
        problems.append(f"vocadito items missing: {sorted(expect - names)}")
    if len([e for e in entries if e["dataset"] == "dcs"]) != 3:
        problems.append("expected 3 dcs items")
    if len([e for e in entries if e["dataset"] == "vocalset"]) != 2:
        problems.append("expected 2 vocalset items (straight only, vowel a first)")
    for e in entries:
        if e["notes"] == 0:
            problems.append(f"{e['name']}: no notes")
    if args.vs_eval:
        result = subprocess.run([args.vs_eval, "--summary-only", "--algorithm", "both", out], capture_output=True, text=True)
        if result.returncode != 0:
            problems.append(f"vs-eval failed: {result.stderr.strip()}")
        else:
            summary = json.loads(result.stdout)["summary"]
            for alg in ("yin", "mpm"):
                rpa = summary[alg]["f0"]["rawPitchAccuracy"]
                f1 = summary[alg]["notes"]["fromRest"]["f1"]
                log(f"vs-eval {alg}: f0 RPA {rpa:.3f}, note F1 from rest {f1:.3f}")
                if rpa < 0.9:
                    problems.append(f"{alg}: raw pitch accuracy {rpa:.3f} < 0.9 on the fake corpus")
    for p in problems:
        log(f"selfcheck: {p}")
    log(f"selfcheck {'FAILED' if problems else 'ok'} ({work})")
    return 1 if problems else 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)

    d = sub.add_parser("download")
    d.add_argument("--raw", required=True)
    d.add_argument("--datasets", default="vocadito,dcs,vocalset")
    d.add_argument("--source", action="append", help="NAME=local path or URL of the archive (skips Zenodo)")
    d.add_argument("--manifest-dir")
    d.add_argument("--work", help="where a whole archive is stored while it is read (default: --raw)")
    d.add_argument("--no-ranges", action="store_true", help="always fetch whole archives")
    d.add_argument("--vocalset-per-context", type=int, default=1)
    d.add_argument("--strict", action="store_true", help="exit 1 if any dataset fails (default: only if all fail)")
    d.set_defaults(func=cmd_download)

    c = sub.add_parser("convert")
    c.add_argument("--raw", required=True)
    c.add_argument("--out", required=True)
    c.add_argument("--datasets", default="vocadito,dcs,vocalset")
    c.add_argument("--rate", type=int, default=44100)
    c.add_argument("--level-db", type=float, default=convert_mod.DEFAULT_LEVEL_DB)
    c.add_argument("--vocadito-notes", choices=("a1", "a2", "f0"), default="a1")
    c.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    c.add_argument("--limit", type=int, default=0, help="at most N files per dataset (quick runs)")
    c.add_argument("--manifest-dir")
    c.set_defaults(func=cmd_convert)

    f = sub.add_parser("fake")
    f.add_argument("--out", required=True)
    f.set_defaults(func=cmd_fake)

    s = sub.add_parser("selfcheck")
    s.add_argument("--work")
    s.add_argument("--vs-eval")
    s.set_defaults(func=cmd_selfcheck)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
