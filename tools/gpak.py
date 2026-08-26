# Minimal reader for Glaiel's .gpak, per GPak::LoadIndex @ 0x140A434D0:
#   u32 count, then per entry { u16 namelen, char name[namelen], u32 size },
#   accumulating a running offset. The data section starts at the stream
#   position immediately after the index (stored at GPak+0x304 in-game).
import struct, sys, io

class GPak:
    def __init__(self, path):
        self.path = path
        self.fh = open(path, "rb")
        n, = struct.unpack("<I", self.fh.read(4))
        self.entries = {}
        off = 0
        for _ in range(n):
            nl, = struct.unpack("<H", self.fh.read(2))
            name = self.fh.read(nl).decode("utf-8", "replace")
            size, = struct.unpack("<I", self.fh.read(4))
            self.entries[name] = (off, size)
            off += size
        self.base = self.fh.tell()

    def read(self, name):
        off, size = self.entries[name]
        self.fh.seek(self.base + off)
        return self.fh.read(size)

if __name__ == "__main__":
    g = GPak(sys.argv[1])
    print(f"{len(g.entries)} entries, data base = {g.base} (0x{g.base:X})")
    tot = sum(s for _, s in g.entries.values())
    print(f"sum of sizes = {tot} ; file size = {g.fh.seek(0,2)}")
    print(f"base+sum     = {g.base+tot}   (should equal file size if the format is right)")
    for pat in sys.argv[2:]:
        hits = [k for k in g.entries if pat.lower() in k.lower()]
        print(f"\n--- {len(hits)} entries matching {pat!r} (first 40)")
        for k in sorted(hits)[:40]:
            print(f"    {k}   {g.entries[k][1]} bytes")
