from pathlib import Path

names = [line.removeprefix("GLD_DELEGATE(").removesuffix(")") for line in Path("GLDDelegate.def").read_text().splitlines()]
lines = [".data", ".align 2"]
for name in names:
    lines.extend((f".globl _XenosStock_{name}", f"_XenosStock_{name}:", "    .long 0"))
lines.append(".text")
for name in names:
    lines.extend((
        ".align 2", f".globl _{name}", f"_{name}:",
        "    mflr r0", f"    bcl 20,31,L_{name}", f"L_{name}:", "    mflr r11", "    mtlr r0",
        f"    addis r11,r11,ha16(_XenosStock_{name}-L_{name})",
        f"    lwz r12,lo16(_XenosStock_{name}-L_{name})(r11)", "    mtctr r12", "    bctr", "",
    ))
Path("build/GLDDelegate.s").write_text("\n".join(lines))
