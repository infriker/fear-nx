"""Adapt one locally generated F.E.A.R. translation unit for the Switch."""
import argparse
from pathlib import Path
import re
import tempfile


# Keep the game's frontbuffer count and Begin/EndTiling clear/resolve sequence.
# Only the 720p scene surface and the submitted rectangle become full height.
FULL_FRAME = {
    "sub_82292900": (
        "\t// ble cr6,0x82292a18\n\tif (!ctx.cr6.gt) goto loc_82292A18;\n\t// lis r11,-13108",
        "\tif (ctx.r29.u32 == 1280 && ctx.r30.u32 == 720 &&\n"
        "\t    ((ctx.r28.u32 == 1 && ctx.r11.u32 == 2) ||\n"
        "\t     (ctx.r28.u32 == 2 && ctx.r11.u32 == 3))) goto loc_82292A18;\n",
    ),
    "sub_822896E8": (
        "\t// bl 0x822894a8\n\tctx.lr = 0x82289850;\n\tsub_822894A8(ctx, base);",
        "\t// Size the depth surface for the same full region as color.\n"
        "\tif (ctx.r27.u32 == 1280 && ctx.r26.u32 == 720 &&\n"
        "\t    ((ctx.r7.u32 == 1 && ctx.r6.u32 == 2) ||\n"
        "\t     (ctx.r7.u32 == 2 && ctx.r6.u32 == 3))) ctx.r6.u64 = 1;\n",
    ),
    "sub_822899B8": (
        "\t// cmplwi cr6,r31,4\n\tctx.cr6.compare<uint32_t>(ctx.r31.u32, 4, ctx.xer);",
        "\t// The non-tiled early return is past. Submit one full rectangle\n"
        "\t// while keeping the guest clear and resolve sequence active.\n"
        "\tif (ctx.r30.u32 == 1280 && ctx.r3.u32 == 720 &&\n"
        "\t    (ctx.r31.u32 == 2 || ctx.r31.u32 == 3)) ctx.r31.u64 = 1;\n",
    ),
}
LAYOUT_UNITS = {91: "sub_822896E8", 136: "sub_82292900", 171: "sub_822899B8"}

# Addresses used by native/wrapped code or excluded during the retail ELF audit.
# Leave these and any calls crossing a nonlocal return on the public entry.
PUBLIC_ENTRIES = set("""
82175610 82175790 82175D78 821763E0 821764F0 82176A60 82176C70 82176EB0 821777C8
8217805C 82178A10 82178A88 82178D1C 82178D70 82178FD4 82179030 8217986C 82182060
822FC710 8230F988 82310860 82310AC0 8234C0A0 8237F648 82387DB8 823B70A8
823E93B0 8241E338 8287B5E0 8287B630 8287B638 8287B650 8287B718 8287B838
82887FD0 828883F8 8288CC70 828920C0
82965DD8 82BB8FA0 82BB9798 82BBA200 82BC0C90
""".split())
FUNCTION = re.compile(r"^DEFINE_REX_FUNC\((\w+)\) \{\n.*?^\}", re.M | re.S)
NONLOCAL = re.compile(
    r"\b(?:\w*(?:setjmp|longjmp)\w*|(?:__imp__)?(?:RtlUnwind\w*|RtlCaptureContext|"
    r"RtlRaiseException|KeBugCheck\w*))\b|coroutines::", re.I)
DIRECT_BL = re.compile(
    r"^([ \t]*// bl 0x([\da-fA-F]{8})\n[ \t]+ctx\.lr = 0x[\da-fA-F]{8};\n[ \t]+)"
    r"(sub_([\da-fA-F]{8}))\(ctx, base\);", re.M)


def adapt(source, unit):
    expected = LAYOUT_UNITS.get(unit)
    sites = []

    def redirect(call):
        address = call[4].upper()
        if address in PUBLIC_ENTRIES or address != call[2].upper():
            return call[0]
        return call[1] + "__imp__" + call[3] + "(ctx, base);"

    def change(function):
        body = function[0]
        if function[1] in FULL_FRAME:
            anchor, prefix = FULL_FRAME[function[1]]
            if body.count(anchor) != 1:
                raise ValueError(f"Unknown full-frame site in {function[1]}")
            body = body.replace(anchor, prefix + anchor)
            sites.append(function[1])
        return body if NONLOCAL.search(body) else DIRECT_BL.sub(redirect, body)

    result = FUNCTION.sub(change, source)
    if sites != ([expected] if expected else []):
        raise ValueError(f"Unexpected full-frame functions in unit {unit}: {sites}")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--deko-cpu", action="store_true", help="Apply the deko3d FPSCR and CAS-region policy")
    args = parser.parse_args()
    unit = int(args.input.name.split(".")[1])
    output = adapt(args.input.read_text(encoding="utf-8"), unit)
    if args.deko_cpu:
        from prepare_cpu_adapter import transform
        output, _, _ = transform(output)
    output = output.encode("utf-8")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_bytes() != output:
        with tempfile.NamedTemporaryFile(dir=args.output.parent, delete=False) as tmp:
            tmp.write(output)
        Path(tmp.name).replace(args.output)
