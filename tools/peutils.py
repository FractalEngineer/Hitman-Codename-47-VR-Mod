"""Minimal PE reader for the mod's tooling.

Only what the project needs: section mapping, the import table, and the export
table. Run directly to list a binary's exports with their RVAs:

    python tools/peutils.py <binary.dll>
"""

import re
import struct
import sys
from pathlib import Path


class PE:
    def __init__(self, data: bytes):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("not a PE file (missing MZ)")
        self.pe_off = struct.unpack_from("<I", data, 0x3C)[0]
        if data[self.pe_off:self.pe_off + 4] != b"PE\0\0":
            raise ValueError("not a PE file (missing PE signature)")

        coff = self.pe_off + 4
        self.machine, self.num_sections = struct.unpack_from("<HH", data, coff)
        size_opt = struct.unpack_from("<H", data, coff + 16)[0]
        self.opt_off = coff + 20

        magic = struct.unpack_from("<H", data, self.opt_off)[0]
        if magic == 0x10B:
            self.is_pe32 = True
            dir_off = self.opt_off + 96
        elif magic == 0x20B:
            self.is_pe32 = False
            dir_off = self.opt_off + 112
        else:
            raise ValueError(f"unknown optional header magic 0x{magic:x}")
        self.data_dirs = dir_off

        sec_off = self.opt_off + size_opt
        self.sections = []
        for i in range(self.num_sections):
            base = sec_off + i * 40
            name = data[base:base + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, base + 8)
            self.sections.append((name, vaddr, vsize, raddr, rsize))

    def rva_to_off(self, rva: int) -> int:
        for _name, vaddr, vsize, raddr, rsize in self.sections:
            span = max(vsize, rsize)
            if vaddr <= rva < vaddr + span:
                return raddr + (rva - vaddr)
        raise ValueError(f"rva 0x{rva:x} is not inside any section")

    def cstr(self, rva: int) -> str:
        off = self.rva_to_off(rva)
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("ascii", "replace")

    def image_base(self) -> int:
        # Optional header fields 28 (PE32) / 24 (PE32+) hold ImageBase.
        if self.is_pe32:
            return struct.unpack_from("<I", self.data, self.opt_off + 28)[0]
        return struct.unpack_from("<Q", self.data, self.opt_off + 24)[0]

    def data_dir(self, index: int):
        off = self.data_dirs + index * 8
        return struct.unpack_from("<II", self.data, off)

    def imports(self):
        """Yield (dll_name_lower, [name or ordinal, ...])."""
        rva, _size = self.data_dir(1)  # IMAGE_DIRECTORY_ENTRY_IMPORT
        if rva == 0:
            return
        off = self.rva_to_off(rva)
        stride_entry = 4 if self.is_pe32 else 8
        high = 1 << (31 if self.is_pe32 else 63)
        while True:
            ilt, _ts, _fc, name_rva, _ft = struct.unpack_from("<IIIII", self.data, off)
            if ilt == 0 and name_rva == 0:
                return
            if ilt == 0:
                off += 20
                continue
            dll = self.cstr(name_rva).lower()
            entries = []
            cur = self.rva_to_off(ilt)
            while True:
                if self.is_pe32:
                    value, = struct.unpack_from("<I", self.data, cur)
                else:
                    value, = struct.unpack_from("<Q", self.data, cur)
                if value == 0:
                    break
                entries.append((value & 0xFFFF) if value & high else self.cstr(value + 2))
                cur += stride_entry
            yield dll, entries
            off += 20

    def _export_directory(self):
        rva, _size = self.data_dir(0)  # IMAGE_DIRECTORY_ENTRY_EXPORT
        if rva == 0:
            return None
        off = self.rva_to_off(rva)
        return struct.unpack_from("<IIHHIIIIIII", self.data, off)

    def exports(self):
        fields = self._export_directory()
        if fields is None:
            return []
        n_names, addr_names = fields[7], fields[9]
        names = []
        for i in range(n_names):
            nrva, = struct.unpack_from("<I", self.data, self.rva_to_off(addr_names) + i * 4)
            names.append(self.cstr(nrva))
        return names

    def exports_with_rva(self):
        """Named exports as (name, rva), sorted by name."""
        fields = self._export_directory()
        if fields is None:
            return []
        n_names, addr_funcs = fields[7], fields[8]
        addr_names, addr_ordinals = fields[9], fields[10]
        names = []
        for i in range(n_names):
            nrva, = struct.unpack_from("<I", self.data, self.rva_to_off(addr_names) + i * 4)
            ordinal, = struct.unpack_from("<H", self.data, self.rva_to_off(addr_ordinals) + i * 2)
            func_rva, = struct.unpack_from("<I", self.data, self.rva_to_off(addr_funcs) + ordinal * 4)
            names.append((self.cstr(nrva), func_rva))
        return sorted(names)


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    path = Path(argv[1])
    pe = PE(path.read_bytes())
    base = pe.image_base()
    exports = pe.exports_with_rva()
    print(f"{path.name}: {len(exports)} exports, image base 0x{base:08X}")
    for name, rva in exports:
        print(f"  {base + rva:08X}  rva {rva:08X}  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
