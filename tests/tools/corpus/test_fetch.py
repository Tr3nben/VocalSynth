"""Subset selection (datasets.py), partial zip reading (remotezip.py) and fetch() (fetch.py), offline:
a local HTTP server with or without Range support stands in for Zenodo."""

from __future__ import annotations

import hashlib
import http.server
import json
import os
import threading
import zipfile

import pytest

from vscorpus import datasets, fetch, remotezip


# --- Subset selection -------------------------------------------------------------------------------------

def test_select_vocadito():
    names = ["vocadito/Audio/vocadito_3.wav", "vocadito/Annotations/F0/vocadito_3_f0.csv",
             "vocadito/Annotations/Notes/vocadito_3_notesA1.csv", "vocadito/Annotations/Lyrics/vocadito_3_lyrics.txt",
             "vocadito/vocadito_metadata.csv", "__MACOSX/vocadito/Audio/._vocadito_3.wav"]
    sel = datasets.select_members("vocadito", names, {})
    assert sel == {
        "vocadito/Audio/vocadito_3.wav": "Audio/vocadito_3.wav",
        "vocadito/Annotations/F0/vocadito_3_f0.csv": "Annotations/F0/vocadito_3_f0.csv",
        "vocadito/Annotations/Notes/vocadito_3_notesA1.csv": "Annotations/Notes/vocadito_3_notesA1.csv",
        "vocadito/vocadito_metadata.csv": "vocadito_metadata.csv",
    }


def test_select_dcs_uses_the_manifest():
    manifest = datasets.load_manifest("dcs")
    wanted = sorted(manifest["members"])
    names = [f"DagstuhlChoirSet_V1.2.3/{w}" for w in wanted] + ["DagstuhlChoirSet_V1.2.3/audio_wav_22050_mono/other.wav"]
    sel = datasets.select_members("dcs", names, manifest)
    assert sorted(sel.values()) == wanted
    # every track of the manifest has its audio and an f0 source in the member list
    for t in manifest["tracks"]:
        assert t["audio"] in manifest["members"]
        assert t["f0_crepe_lrx"] in manifest["members"] and t["f0_pyin_lrx"] in manifest["members"]
        assert t["id"].split("_")[-1][0] in "SATB"


def test_select_vocalset_straight_vowel_a_first():
    names = ["FULL/m1/scales/straight/m1_scales_straight_e.wav", "FULL/m1/scales/straight/m1_scales_straight_a.wav",
             "FULL/m1/scales/vibrato/m1_scales_vibrato_a.wav", "FULL/m1/arpeggios/straight/m1_arpeggios_straight_o.wav",
             "FULL/f3/arpeggios/belt/f3_arpeggios_belt_a.wav", "by_technique/straight/m1_scales_straight_a.wav",
             "__MACOSX/FULL/m1/scales/straight/._m1_scales_straight_a.wav"]
    sel = datasets.select_members("vocalset", names, {})
    assert sel == {"FULL/m1/scales/straight/m1_scales_straight_a.wav": "m1/m1_scales_straight_a.wav",
                   "FULL/m1/arpeggios/straight/m1_arpeggios_straight_o.wav": "m1/m1_arpeggios_straight_o.wav"}
    two = datasets.select_members("vocalset", names, {}, vocalset_per_context=2)
    assert "m1/m1_scales_straight_e.wav" in two.values()


def test_manifests_are_consistent():
    for name, ds in datasets.DATASETS.items():
        m = datasets.load_manifest(name)
        assert m["dataset"] == name and m["zenodo_record"] == ds.record
        if ds.zip_name:
            assert m["zip"] == ds.zip_name and len(m["zip_md5"]) == 32


# --- Local HTTP server ------------------------------------------------------------------------------------

class _Handler(http.server.BaseHTTPRequestHandler):
    ranges = True
    root = ""
    log = []

    def log_message(self, *args):  # quiet
        pass

    def do_GET(self):  # noqa: N802
        path = os.path.join(self.root, self.path.split("?")[0].lstrip("/"))
        if not os.path.isfile(path):
            self.send_error(404)
            return
        with open(path, "rb") as f:
            data = f.read()
        rng = self.headers.get("Range")
        type(self).log.append(rng)
        if rng and self.ranges:
            start, _, end = rng.removeprefix("bytes=").partition("-")
            start = int(start)
            end = min(int(end) if end else len(data) - 1, len(data) - 1)
            body = data[start : end + 1]
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
        else:
            body = data
            self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


@pytest.fixture
def server(tmp_path):
    def start(ranges: bool):
        handler = type("H", (_Handler,), {"ranges": ranges, "root": str(tmp_path), "log": []})
        httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        servers.append(httpd)
        return f"http://127.0.0.1:{httpd.server_address[1]}", handler

    servers = []
    yield start
    for s in servers:
        s.shutdown()


def _make_zip(path, members):
    with zipfile.ZipFile(path, "w") as zf:
        for k, (name, data) in enumerate(members.items()):
            zf.writestr(name, data, compress_type=zipfile.ZIP_STORED if k % 2 else zipfile.ZIP_DEFLATED)


def test_remotezip_reads_members_by_range(tmp_path, server, monkeypatch):
    monkeypatch.delenv("HTTP_PROXY", raising=False)
    monkeypatch.setenv("NO_PROXY", "127.0.0.1")
    members = {f"top/dir/file{k}.bin": os.urandom(100_000 + 977 * k) for k in range(6)}
    members["top/ü-utf8.txt"] = b"x" * 100
    _make_zip(tmp_path / "a.zip", members)
    url, handler = server(True)
    src = remotezip.Source(f"{url}/a.zip")
    entries = remotezip.list_members(src)
    assert set(entries) == set(members)
    for name in ("top/dir/file3.bin", "top/ü-utf8.txt"):
        assert remotezip.read_member(src, entries[name]) == members[name]
    # central directory (tail of at most 64 KB + 42 bytes) + the two members only
    assert src.bytes_read < 65536 + 42 + 100_000 + 3 * 977 + 100 + 2 * 2048
    # a corrupted CRC is detected
    bad = remotezip.Entry(**{**entries["top/dir/file1.bin"].__dict__, "crc": 0})
    with pytest.raises(ValueError, match="CRC32"):
        remotezip.read_member(src, bad)


def test_remotezip_without_range_support(tmp_path, server, monkeypatch):
    monkeypatch.setenv("NO_PROXY", "127.0.0.1")
    _make_zip(tmp_path / "a.zip", {"a.txt": b"hello"})
    url, _ = server(False)
    with pytest.raises(remotezip.NoRangeSupport):
        remotezip.Source(f"{url}/a.zip")
    dest = tmp_path / "copy.zip"
    md5 = hashlib.md5((tmp_path / "a.zip").read_bytes()).hexdigest()
    assert remotezip.download(f"{url}/a.zip", str(dest), md5) == md5
    with pytest.raises(ValueError, match="md5"):
        remotezip.download(f"{url}/a.zip", str(tmp_path / "x.zip"), "0" * 32)


# --- fetch() with the fake datasets ----------------------------------------------------------------------

@pytest.mark.parametrize("ranges", [True, False])
def test_fetch_dcs_over_http(tmp_path, server, monkeypatch, ranges):
    monkeypatch.setenv("NO_PROXY", "127.0.0.1")
    from vscorpus import fake

    paths = fake.make_fake_sources(str(tmp_path / "src"))
    url, handler = server(ranges)
    rel = os.path.relpath(paths["dcs"], tmp_path)
    info = fetch.fetch("dcs", str(tmp_path / "raw"), f"{url}/{rel}", str(tmp_path / "src" / "manifests"))
    assert info["method"] == ("range" if ranges else "full")
    assert len(info["files"]) == 11
    assert not [f for f in os.listdir(tmp_path / "raw") if f.startswith(".")]  # whole archive deleted again
    # second call: cache hit, nothing requested
    handler.log.clear()
    again = fetch.fetch("dcs", str(tmp_path / "raw"), f"{url}/{rel}", str(tmp_path / "src" / "manifests"))
    assert again["files"] == info["files"] and handler.log == []
    # a changed file invalidates the cache
    first = next(iter(info["files"]))
    with open(tmp_path / "raw" / "dcs" / first, "ab") as f:
        f.write(b"x")
    assert not fetch._cache_ok(str(tmp_path / "raw" / "dcs"))


def test_fetch_rejects_a_wrong_member_md5(tmp_path):
    from vscorpus import fake

    paths = fake.make_fake_sources(str(tmp_path / "src"))
    mpath = tmp_path / "src" / "manifests" / "vocadito.json"
    m = json.loads(mpath.read_text())
    m["members"]["Audio/vocadito_1.wav"] = "0" * 32
    m["zip_md5"] = None
    mpath.write_text(json.dumps(m))
    with pytest.raises(RuntimeError, match="md5"):
        fetch.fetch("vocadito", str(tmp_path / "raw"), paths["vocadito"], str(tmp_path / "src" / "manifests"))


def test_resolve_archive_prefers_the_named_or_smallest(monkeypatch):
    listing = [{"key": "VocalSet_by_technique.zip", "size": 6_000_000_000, "md5": "a" * 32, "url": "u1"},
               {"key": "VocalSet1-2.zip", "size": 2_100_000_000, "md5": "b" * 32, "url": "u2"},
               {"key": "README.txt", "size": 10, "md5": "c" * 32, "url": "u3"}]
    monkeypatch.setattr(fetch, "zenodo_files", lambda record: listing)
    assert fetch.resolve_archive("vocalset", {})["key"] == "VocalSet1-2.zip"
    monkeypatch.setattr(fetch, "zenodo_files", lambda record: [listing[0], {**listing[1], "key": "vocalset_small.zip"}])
    assert fetch.resolve_archive("vocalset", {})["key"] == "vocalset_small.zip"

    def offline(record):
        raise OSError("no network")

    monkeypatch.setattr(fetch, "zenodo_files", offline)
    assert fetch.resolve_archive("vocalset", {})["key"] == "VocalSet1-2.zip"
    dcs = fetch.resolve_archive("dcs", datasets.load_manifest("dcs"))
    assert dcs["key"] == "DagstuhlChoirSet_V1.2.3.zip" and dcs["md5_pinned"]
