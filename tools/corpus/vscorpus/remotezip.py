"""Reads single members of a (possibly huge, possibly remote) zip archive. Standard library only.

A remote archive is read with HTTP Range requests: the end of the file (central directory) first, then
exactly the bytes of each wanted member, so a 5 GB archive costs a few MB when only a subset is needed.
If the server does not answer a Range request with 206, the caller falls back to download() of the whole
archive and reads the members from the local file with the same code (docs/CORPUS.md, "Download").

Supports stored and deflated members, ZIP64, UTF-8 names. Other compression methods (e.g. Deflate64 from
Windows) raise UnsupportedMember; for a local archive extract_with_unzip() then uses the unzip tool.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import struct
import subprocess
import time
import urllib.error
import urllib.request
import zlib
from dataclasses import dataclass

USER_AGENT = "VocalSynth-corpus/1.0 (+https://github.com; docs/CORPUS.md)"
RETRIES = 8


class UnsupportedMember(Exception):
    pass


class NoRangeSupport(Exception):
    pass


@dataclass
class Entry:
    name: str
    method: int
    flags: int
    crc: int
    compressed_size: int
    size: int
    header_offset: int


def _log(message: str) -> None:
    import sys

    print(message, file=sys.stderr, flush=True)


def _sleep_for(error: Exception, attempt: int) -> float:
    if isinstance(error, urllib.error.HTTPError) and error.code == 429:
        try:
            return max(1.0, float(error.headers.get("Retry-After", "10")))
        except ValueError:
            return 10.0
    return min(60.0, 2.0 ** attempt)


def _retryable(error: Exception) -> bool:
    if isinstance(error, urllib.error.HTTPError):
        return error.code in (408, 425, 429, 500, 502, 503, 504)
    return isinstance(error, (urllib.error.URLError, TimeoutError, ConnectionError, OSError))


class Source:
    """A zip archive at a local path or an http(s) URL; read_range() returns exact byte ranges."""

    def __init__(self, location: str, timeout: float = 120.0):
        self.location = location
        self.timeout = timeout
        self.is_local = not location.startswith(("http://", "https://"))
        self.requests = 0
        self.bytes_read = 0
        self._url = location
        if self.is_local:
            self._size = os.path.getsize(location)
        else:
            self._size = self._resolve()

    @property
    def size(self) -> int:
        return self._size

    def _request(self, url: str, start: int, end: int):
        req = urllib.request.Request(url, headers={"Range": f"bytes={start}-{end}", "User-Agent": USER_AGENT})
        return urllib.request.urlopen(req, timeout=self.timeout)

    def _resolve(self) -> int:
        """Follows redirects once; raises NoRangeSupport unless the server answers 206 with a total size."""
        for attempt in range(RETRIES):
            try:
                with self._request(self.location, 0, 0) as resp:
                    self.requests += 1
                    if resp.status != 206:
                        raise NoRangeSupport(f"{self.location}: HTTP {resp.status} to a Range request")
                    content_range = resp.headers.get("Content-Range", "")
                    total = content_range.rpartition("/")[2]
                    if not total.isdigit():
                        raise NoRangeSupport(f"{self.location}: no total size in Content-Range {content_range!r}")
                    self._url = resp.geturl()
                    resp.read()
                    return int(total)
            except NoRangeSupport:
                raise
            except Exception as e:  # noqa: BLE001 - network errors are retried
                if not _retryable(e) or attempt == RETRIES - 1:
                    raise
                delay = _sleep_for(e, attempt)
                _log(f"  retry in {delay:.0f} s ({e})")
                time.sleep(delay)
        raise RuntimeError("unreachable")

    def read_range(self, start: int, length: int) -> bytes:
        if length <= 0:
            return b""
        end = min(self._size, start + length) - 1
        if self.is_local:
            with open(self.location, "rb") as f:
                f.seek(start)
                data = f.read(end - start + 1)
            self.bytes_read += len(data)
            return data

        refreshed = False
        for attempt in range(RETRIES):
            try:
                with self._request(self._url, start, end) as resp:
                    self.requests += 1
                    if resp.status != 206:
                        raise NoRangeSupport(f"HTTP {resp.status} to a Range request")
                    data = resp.read()
                if len(data) != end - start + 1:
                    raise ConnectionError(f"short read {len(data)} of {end - start + 1} bytes")
                self.bytes_read += len(data)
                return data
            except NoRangeSupport:
                raise
            except urllib.error.HTTPError as e:
                # Signed redirect targets can expire: resolve the original URL again once
                if e.code in (401, 403, 404, 410) and not refreshed:
                    refreshed = True
                    self._resolve()
                    continue
                if not _retryable(e) or attempt == RETRIES - 1:
                    raise
                delay = _sleep_for(e, attempt)
            except Exception as e:  # noqa: BLE001
                if not _retryable(e) or attempt == RETRIES - 1:
                    raise
                delay = _sleep_for(e, attempt)
            _log(f"  retry in {delay:.0f} s (range {start}-{end})")
            time.sleep(delay)
        raise RuntimeError("unreachable")


# --- Central directory ------------------------------------------------------------------------------------

_EOCD = b"PK\x05\x06"
_ZIP64_LOCATOR = b"PK\x06\x07"
_ZIP64_EOCD = b"PK\x06\x06"
_CENTRAL = b"PK\x01\x02"
_LOCAL = b"PK\x03\x04"


def list_members(src: Source) -> dict[str, Entry]:
    tail_length = min(src.size, 65536 + 22 + 20)
    tail_start = src.size - tail_length
    tail = src.read_range(tail_start, tail_length)
    pos = tail.rfind(_EOCD)
    if pos < 0:
        raise ValueError(f"{src.location}: not a zip archive (no end of central directory)")

    (_, _, _, _, entries, cd_size, cd_offset, _) = struct.unpack("<4sHHHHIIH", tail[pos : pos + 22])

    if entries == 0xFFFF or cd_size == 0xFFFFFFFF or cd_offset == 0xFFFFFFFF:
        loc = pos - 20
        if loc < 0 or tail[loc : loc + 4] != _ZIP64_LOCATOR:
            raise ValueError(f"{src.location}: ZIP64 locator missing")
        (_, _, zip64_offset, _) = struct.unpack("<4sIQI", tail[loc : loc + 20])
        record = src.read_range(zip64_offset, 56)
        if record[:4] != _ZIP64_EOCD:
            raise ValueError(f"{src.location}: ZIP64 end of central directory missing")
        (_, _, _, _, _, _, _, entries, cd_size, cd_offset) = struct.unpack("<4sQHHIIQQQQ", record)

    cd = src.read_range(cd_offset, cd_size)
    members: dict[str, Entry] = {}
    p = 0
    for _ in range(entries):
        if cd[p : p + 4] != _CENTRAL:
            raise ValueError(f"{src.location}: corrupt central directory at entry {len(members)}")
        (_, _, _, flags, method, _, _, crc, csize, usize, nlen, elen, clen, _, _, _, offset) = struct.unpack(
            "<4sHHHHHHIIIHHHHHII", cd[p : p + 46]
        )
        raw_name = cd[p + 46 : p + 46 + nlen]
        extra = cd[p + 46 + nlen : p + 46 + nlen + elen]
        name = raw_name.decode("utf-8" if flags & 0x800 else "cp437", errors="replace")

        # ZIP64 extended information: only the fields that are 0xFFFFFFFF, in this order
        q = 0
        while q + 4 <= len(extra):
            tag, size = struct.unpack("<HH", extra[q : q + 4])
            if tag == 0x0001:
                field = extra[q + 4 : q + 4 + size]
                f = 0
                if usize == 0xFFFFFFFF:
                    (usize,) = struct.unpack("<Q", field[f : f + 8])
                    f += 8
                if csize == 0xFFFFFFFF:
                    (csize,) = struct.unpack("<Q", field[f : f + 8])
                    f += 8
                if offset == 0xFFFFFFFF:
                    (offset,) = struct.unpack("<Q", field[f : f + 8])
            q += 4 + size

        if not name.endswith("/"):
            members[name] = Entry(name, method, flags, crc, csize, usize, offset)
        p += 46 + nlen + elen + clen
    return members


def read_member(src: Source, entry: Entry) -> bytes:
    if entry.flags & 0x1:
        raise UnsupportedMember(f"{entry.name}: encrypted")
    if entry.method not in (0, 8):
        raise UnsupportedMember(f"{entry.name}: compression method {entry.method}")

    slack = 30 + len(entry.name.encode("utf-8")) + 1024
    blob = src.read_range(entry.header_offset, slack + entry.compressed_size)
    if blob[:4] != _LOCAL:
        raise ValueError(f"{entry.name}: local header missing")
    nlen, elen = struct.unpack("<HH", blob[26:30])
    start = 30 + nlen + elen
    if start + entry.compressed_size > len(blob):  # unusually long local extra field
        blob = src.read_range(entry.header_offset, start + entry.compressed_size)
    data = blob[start : start + entry.compressed_size]

    if entry.method == 8:
        d = zlib.decompressobj(-15)
        data = d.decompress(data) + d.flush()
    if len(data) != entry.size:
        raise ValueError(f"{entry.name}: size {len(data)} != {entry.size}")
    if zlib.crc32(data) & 0xFFFFFFFF != entry.crc:
        raise ValueError(f"{entry.name}: CRC32 mismatch")
    return data


def extract_member(src: Source, entry: Entry, dest: str) -> None:
    data = read_member(src, entry)
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    tmp = dest + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, dest)


def extract_with_unzip(zip_path: str, member: str, dest: str) -> None:
    """Fallback for compression methods zlib cannot read (unzip 6 reads Deflate64)."""
    if shutil.which("unzip") is None:
        raise UnsupportedMember(f"{member}: needs the unzip tool")
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    with open(dest + ".part", "wb") as out:
        subprocess.run(["unzip", "-p", zip_path, member], stdout=out, check=True)
    os.replace(dest + ".part", dest)


# --- Whole-file download ---------------------------------------------------------------------------------


def download(url: str, dest: str, expected_md5: str | None = None, expected_size: int | None = None) -> str:
    """Streams url to dest (resuming with Range after a broken connection where possible); returns the md5.
    Raises ValueError on a size or md5 mismatch."""
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    tmp = dest + ".part"
    h = hashlib.md5()
    done = 0
    total = expected_size
    last_report = time.monotonic()
    started = last_report
    with open(tmp, "wb") as out:
        for attempt in range(RETRIES):
            try:
                headers = {"User-Agent": USER_AGENT}
                if done > 0:
                    headers["Range"] = f"bytes={done}-"
                req = urllib.request.Request(url, headers=headers)
                with urllib.request.urlopen(req, timeout=300) as resp:
                    if done > 0 and resp.status != 206:  # no resume: start over
                        out.seek(0)
                        out.truncate()
                        h = hashlib.md5()
                        done = 0
                    if total is None and resp.headers.get("Content-Length") and done == 0:
                        total = int(resp.headers["Content-Length"])
                    while True:
                        block = resp.read(1 << 20)
                        if not block:
                            break
                        out.write(block)
                        h.update(block)
                        done += len(block)
                        now = time.monotonic()
                        if now - last_report > 30:
                            rate = done / max(1e-3, now - started) / 1e6
                            share = f"{100.0 * done / total:.0f} %" if total else "?"
                            _log(f"  {os.path.basename(dest)}: {done / 1e6:.0f} MB ({share}, {rate:.1f} MB/s)")
                            last_report = now
                if total is not None and done < total:
                    raise ConnectionError(f"connection closed at {done} of {total} bytes")
                break
            except Exception as e:  # noqa: BLE001
                if not _retryable(e) or attempt == RETRIES - 1:
                    raise
                delay = _sleep_for(e, attempt)
                _log(f"  download retry in {delay:.0f} s at {done} bytes ({e})")
                time.sleep(delay)

    md5 = h.hexdigest()
    if expected_size is not None and done != expected_size:
        raise ValueError(f"{dest}: {done} bytes, expected {expected_size}")
    if expected_md5 and md5 != expected_md5:
        raise ValueError(f"{dest}: md5 {md5}, expected {expected_md5}")
    os.replace(tmp, dest)
    return md5
