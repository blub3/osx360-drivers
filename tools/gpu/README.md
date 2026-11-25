# GPU host tools

Run on the PC with Python 3. They read Apple binaries copied off the Xbox; keep those binaries out of the repository.

- `macho.py <binary> <hex address>...` describes what is at an address in a 32-bit PowerPC Mach-O file: a C string, a CFString, or the section.
- `annotate-ppc.py <binary> <otool -tV output>` adds the strings and CFStrings that position-independent PowerPC code loads (`bcl`/`mflr` base plus `addis` and `addi`/`lwz`) to an `otool -arch ppc -tV` listing made on the G5.

Example and results: `docs/gpu-tiger-gl-stack.md`.
