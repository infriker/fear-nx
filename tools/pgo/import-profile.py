"""Unpack one completed GCC 15.2 Switch snapshot for profile-guided compilation."""
import argparse
from pathlib import Path
import struct


def unpack(data):
    expected = {f"fear_xbox360_recomp.{i}.cpp.gcda" for i in range(199)}
    units = {}
    highest = 0
    pos = 0

    def take(length):
        nonlocal pos
        if length < 0 or length > len(data) - pos:
            raise ValueError(f"Truncated profile at byte {pos}")
        start = pos
        pos += length
        return data[start:pos]

    def integer():
        return struct.unpack("<i", take(4))[0]

    while pos < len(data):
        if take(8) != b"nfcg*25B":
            raise ValueError("Expected a GCC 15.2 filename record")
        name_length = integer()
        if not 1 < name_length <= 255:
            raise ValueError("Invalid unit name length")
        raw_name = take(name_length)
        name = raw_name[:-1].decode("ascii")
        if not raw_name.endswith(b"\0") or name not in expected or name in units:
            raise ValueError(f"Unexpected or duplicate unit: {name!r}")
        start = pos
        if take(16)[:8] != b"adcg*25B":
            raise ValueError(f"Invalid GCC 15.2 data for {name}")
        while tag := integer():
            length = integer()
            payload = take(max(0, length))  # Negative length encodes zero counters.
            if tag == 0x01A10000:
                if length % 8:
                    raise ValueError("Invalid branch counter length")
                for count, in struct.iter_unpack("<Q", payload):
                    highest = max(highest, count)
        units[name] = data[start:pos]
    if units.keys() != expected or not highest:
        raise ValueError("Expected a complete recording with branches executed in gameplay")
    # libgcov's freestanding stream omits OBJECT_SUMMARY.
    summary = struct.pack("<IIII", 0xa1000000, 8, 1, min(highest, 0xffffffff))
    return {name: raw[:16] + summary + raw[16:] for name, raw in units.items()}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    units = unpack(args.recording.read_bytes())
    args.destination.mkdir(parents=True, exist_ok=True)
    for name, contents in units.items():
        output = args.destination / name
        temporary = output.with_suffix(".gcda.tmp")
        temporary.write_bytes(contents)
        temporary.replace(output)
    print(f"Imported {len(units)} gameplay profile units.")
