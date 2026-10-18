"""Search plain (non-texture) files inside a HashFS v2 .scs archive for a byte pattern.

Usage: python -I hashfs_grep.py <archive.scs> <pattern> [out_dir]
Prints each matching entry's hash, size and the matching lines; with out_dir,
also writes the full matching files there as <hash>.txt.
Format per sk-zk/TruckLib.HashFs (HashFsV2Reader.cs).
"""

import struct
import sys
import zlib
from pathlib import Path

PLAIN = 128
MAX_SIZE = 4 << 20


def main() -> None:
    archive, pattern = Path(sys.argv[1]), sys.argv[2].encode()
    out_dir = Path(sys.argv[3]) if len(sys.argv) > 3 else None
    with open(archive, "rb") as f:
        hdr = f.read(4 + 2 + 2 + 4 + 4 + 4 + 4 + 4 + 8 + 8 + 4 + 1)
        (magic, version, _salt, method, n_entries, et_len, _n_meta, mt_len, et_start, mt_start, _sec,
         _platform) = struct.unpack("<IHH4sIIIIQQIB", hdr)
        if magic != 0x23534353 or version != 2:
            sys.exit(f"{archive}: not a HashFS v2 archive (version {version})")
        f.seek(et_start)
        entry_table = zlib.decompress(f.read(et_len))
        f.seek(mt_start)
        meta = zlib.decompress(f.read(mt_len))

        hits = 0
        scanned = 0
        for i in range(n_entries):
            h, meta_index, meta_count, _flags = struct.unpack_from("<QIHH", entry_table, i * 16)
            pos = meta_index * 4
            first_type = meta[pos + 3]
            if first_type != PLAIN:
                continue
            pos += meta_count * 4
            c0, c1, c2, c3, s0, s1, s2, s3, _unk, block = struct.unpack_from("<8BII", meta, pos)
            csize = c0 | c1 << 8 | c2 << 16 | (c3 & 0x0F) << 24
            size = s0 | s1 << 8 | s2 << 16 | (s3 & 0x0F) << 24
            compressed = bool(c3 & 0x10)
            if size == 0 or size > MAX_SIZE:
                continue
            f.seek(block * 16)
            raw = f.read(csize if compressed else size)
            try:
                data = zlib.decompress(raw) if compressed else raw
            except zlib.error:
                continue
            scanned += 1
            if pattern not in data:
                continue
            hits += 1
            print(f"== entry {h:016x} size {size}")
            for line in data.split(b"\n"):
                if pattern in line:
                    print("   ", line.strip().decode("utf-8", "replace")[:200])
            if out_dir:
                out_dir.mkdir(parents=True, exist_ok=True)
                (out_dir / f"{h:016x}.txt").write_bytes(data)
        print(f"{archive.name}: scanned {scanned} plain entries, {hits} matched", file=sys.stderr)


if __name__ == "__main__":
    main()
