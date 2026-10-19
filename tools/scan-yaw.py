"""Find the player's yaw field in the running game, and measure degrees per pixel.

No eyeballing and no image matching: take three snapshots of the process's
writable memory, inject the same pixel turn twice, and keep the floats whose
change is identical across both turns. A field that advances by exactly the same
amount for the same input is the one the input drives - for the camera, that is
the yaw.

The delta is already in radians, so the scale falls straight out:

    degrees per pixel = delta_radians * 57.29578 / pixels

Usage:
    python tools/scan-yaw.py --pixels 100

Do not move the physical mouse while it runs; it feeds the same accumulator.
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

import numpy as np

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
MEM_COMMIT = 0x1000
MEM_PRIVATE = 0x20000
# READWRITE, WRITECOPY, EXECUTE_READWRITE, EXECUTE_WRITECOPY
WRITABLE = (0x04, 0x08, 0x40, 0x80)
MAX_REGION = 64 * 1024 * 1024
RAD2DEG = 57.29577951308232


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wt.DWORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
    ]


def open_game():
    pid = None
    for line in os.popen('tasklist /FI "IMAGENAME eq Hitman.Exe" /FO CSV /NH'):
        parts = [p.strip('"') for p in line.strip().split('","')]
        if len(parts) > 1 and parts[0].lower() == "hitman.exe":
            pid = int(parts[1])
            break
    if pid is None:
        sys.exit("Hitman is not running")
    handle = ctypes.windll.kernel32.OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not handle:
        sys.exit("OpenProcess failed")
    return pid, handle


def regions(handle):
    info = MEMORY_BASIC_INFORMATION()
    address = 0
    while address < 0x7FFF0000:
        written = ctypes.windll.kernel32.VirtualQueryEx(
            handle, ctypes.c_void_p(address), ctypes.byref(info), ctypes.sizeof(info))
        if written == 0:
            break
        base = int(info.BaseAddress or 0)
        size = int(info.RegionSize)
        if (info.State == MEM_COMMIT and info.Type == MEM_PRIVATE
                and info.Protect in WRITABLE and 0 < size <= MAX_REGION):
            yield base, size
        if size == 0:
            break
        address = base + size


def snapshot(handle):
    data = {}
    for base, size in regions(handle):
        chunk = ctypes.create_string_buffer(size)
        read = ctypes.c_size_t(0)
        ok = ctypes.windll.kernel32.ReadProcessMemory(
            handle, ctypes.c_void_p(base), chunk, size, ctypes.byref(read))
        if ok and read.value == size:
            data[base] = (size, chunk.raw)
    return data


def request_turn(game_dir, pixels):
    path = os.path.join(game_dir, "h47vr-look.txt")
    with open(path, "w", encoding="ascii") as handle:
        handle.write("dx=%d dy=0 steps=10\n" % pixels)
    deadline = time.time() + 3.0
    while os.path.exists(path) and time.time() < deadline:
        time.sleep(0.05)
    time.sleep(0.4)


def as_floats(raw, base, size):
    start = (-base) % 4
    count = (size - start) // 4
    return np.frombuffer(raw, dtype=np.uint8, count=count * 4, offset=start).view("<f4")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pixels", type=int, default=100)
    parser.add_argument("--game", default=r"E:\SteamLibrary\steamapps\common\Hitman Codename 47")
    parser.add_argument("--min", type=float, default=0.01)
    parser.add_argument("--max", type=float, default=1.2)
    parser.add_argument("--save", default=None,
                        help="write 'address value delta' for every candidate")
    args = parser.parse_args()

    pid, handle = open_game()
    print("pid", pid)

    first = snapshot(handle)
    print("snapshot 1: %d regions, %.1f MB"
          % (len(first), sum(v[0] for v in first.values()) / 1e6))
    request_turn(args.game, args.pixels)
    second = snapshot(handle)
    request_turn(args.game, args.pixels)
    third = snapshot(handle)

    found = []
    for base, (size, raw_a) in first.items():
        if base not in second or base not in third:
            continue
        a = as_floats(raw_a, base, size)
        b = as_floats(second[base][1], base, size)
        c = as_floats(third[base][1], base, size)
        d1 = b - a
        d2 = c - b
        with np.errstate(invalid="ignore", over="ignore"):
            mask = (np.abs(d1 - d2) < 1e-4)
            mask &= (np.abs(d1) > args.min) & (np.abs(d1) < args.max)
            mask &= np.isfinite(a) & np.isfinite(b) & np.isfinite(c)
        for index in np.nonzero(mask)[0]:
            found.append((base + int(index) * 4, float(a[index]), float(d1[index])))

    print("\n%d candidate field(s)" % len(found))
    for address, value, delta in found[:40]:
        print("  %08x  value %12.6f  delta %+0.5f rad = %+6.1f deg  (%.5f deg/px)"
              % (address, value, delta, delta * RAD2DEG,
                 delta * RAD2DEG / args.pixels))
    if args.save:
        with open(args.save, "w", encoding="utf-8") as handle:
            for address, value, delta in found:
                handle.write("%08x %r %r\n" % (address, value, delta))
        print("saved %d candidate(s) to %s" % (len(found), args.save))
    ctypes.windll.kernel32.CloseHandle(handle)


if __name__ == "__main__":
    main()
