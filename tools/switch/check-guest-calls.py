"""Reject a link that would let a direct guest BL bypass a native override."""
import argparse
from pathlib import Path
import re
import subprocess


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("nm")
parser.add_argument("elf")
parser.add_argument("guest_dir", type=Path)
args = parser.parse_args()
symbols = {}
listing = subprocess.check_output([args.nm, "--defined-only", args.elf], text=True)
for address, _, symbol in re.findall(r"^([\da-fA-F]+) (\w) ((?:__imp__)?sub_[\da-fA-F]+)$", listing, re.M):
    symbols[symbol] = int(address, 16)

targets = set()
for unit in range(199):
    path = args.guest_dir / f"fear_xbox360_recomp.{unit}.cpp"
    targets.update(re.findall(r"^\s*__imp__(sub_[\da-fA-F]+)\(ctx, base\);", path.read_text(), re.M))
if not targets:
    raise SystemExit("No direct guest calls found")
for name in sorted(targets):
    if name not in symbols or symbols[name] != symbols.get("__imp__" + name):
        raise SystemExit(f"Direct call bypasses {name}. Add its address to PUBLIC_ENTRIES and rebuild.")
print(f"Verified {len(targets)} direct guest call targets against the linked ELF.")
