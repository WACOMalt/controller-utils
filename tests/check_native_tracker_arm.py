#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from build_native_tracker import CAVE, CONFIG_SITE, CONTROL, MODE_SITE, ROOT, SITES


def check(built):
    manifest = json.loads((built / "adapter.json").read_text())
    with tempfile.TemporaryDirectory(prefix="controller-utils-arm-") as directory:
        folder = Path(directory)
        lines = [
            '.section .start,"ax"',
            ".global _start",
            "_start:",
            "ldr x9, =control",
            "mov w10, #0",
            "str w10, [x9, #12]",
            "bl run",
            "ldr x9, =control",
            "mov w10, #1",
            "str w10, [x9, #12]",
            "bl run",
            "bl run",
            "ldr x0, =control",
            "bl check_loader_control",
            "cbz w0, error",
            "adr x9, checks",
            "ldr w9, [x9]",
            "cmp w9, #3",
            "b.ne error",
            "adr x9, loads",
            "ldr w9, [x9]",
            "cmp w9, #1",
            "b.ne error",
            "bl check_modes",
            "bl check_shared",
            "mov x0, #0",
            "b exit",
            "error:",
            "mov x0, #1",
            "exit:",
            "mov x8, #93",
            "svc #0",
            "run:",
            "sub sp, sp, #32",
            "stp x29, x30, [sp, #16]",
            "mov x9, #0x88",
            "mov x10, #0x99",
            "stp x9, x10, [sp]",
        ]
        lines += [f"mov x{i}, #{16 + i}" for i in range(9)]
        lines += [f"movi v{i}.16b, #{32 + i}" for i in range(8)]
        lines += ["bl route_0", "ldp x29, x30, [sp, #16]", "add sp, sp, #32", "ret", "checked:"]
        for i in range(9):
            lines += [f"cmp x{i}, #{16 + i}", "b.ne error"]
        lines += [
            "ldp x9, x10, [sp]",
            "cmp x9, #0x88",
            "b.ne error",
            "cmp x10, #0x99",
            "b.ne error",
        ]
        for i in range(8):
            lines += [
                f"movi v16.16b, #{32 + i}",
                f"cmeq v17.16b, v{i}.16b, v16.16b",
                "uminv b18, v17.16b",
                "umov w9, v18.b[0]",
                "cmp w9, #255",
                "b.ne error",
            ]
        lines += [
            "adr x9, checks",
            "ldr w10, [x9]",
            "add w10, w10, #1",
            "str w10, [x9]",
            "ret",
            "check_shared:",
            "stp x29, x30, [sp, #-16]!",
        ]
        for phase in range(2):
            for route in range(6, len(SITES)):
                lines += [f"mov x{i}, #{16 + i}" for i in range(6)]
                lines += [f"bl route_{route}"]
            if phase == 0:
                lines += ["ldr x9, =control", "str xzr, [x9, #24]"]
        lines += [
            "adr x9, shared_checks",
            "ldr w9, [x9]",
            f"cmp w9, #{2 * (len(SITES) - 6)}",
            "b.ne error",
            "ldp x29, x30, [sp], #16",
            "ret",
            "checked_shared:",
        ]
        for i in range(6):
            lines += [f"cmp x{i}, #{16 + i}", "b.ne error"]
        lines += [
            "adr x9, shared_checks",
            "ldr w10, [x9]",
            "add w10, w10, #1",
            "str w10, [x9]",
            "ret",
            "check_modes:",
            "sub sp, sp, #32",
            "stp x19, x20, [sp]",
            "str x30, [sp, #16]",
            "ldr x16, =control",
            "mov x19, #0x1110",
            "str x19, [x16, #88]",
            "mov x19, #0x2220",
            "mov w20, #1",
            "bl route_mode",
            "cmp w9, #1",
            "b.ne error",
            "cmp w20, #1",
            "b.ne error",
            "mov x19, #0x1110",
            "mov w20, #1",
            "bl route_mode",
            "cbnz w9, error",
            "cbnz w20, error",
            "mov x17, #0x1110",
            "cmp x19, x17",
            "b.ne error",
            "ldr x16, =control",
            "str xzr, [x16, #88]",
            "mov x19, #0",
            "mov w20, #1",
            "bl route_mode",
            "cmp w9, #1",
            "b.ne error",
            "ldp x19, x20, [sp]",
            "ldr x30, [sp, #16]",
            "add sp, sp, #32",
            "ret",
            "fake_attach:",
            "ldr x9, =control",
            "cmp x0, x9",
            "b.ne error",
            "cmp x1, #16",
            "b.ne error",
            "ldr x10, [x2]",
            "ldr x11, =trampoline_0",
            "cmp x10, x11",
            "b.ne error",
            "adr x10, checked",
            "str x10, [x0, #32]",
            "adr x10, shared_callbacks",
            "str x10, [x0, #24]",
            "ret",
            ".ltorg",
            '.section .dlopen,"ax"',
            "cmp w1, #2",
            "b.ne open_error",
            "ldrb w9, [x0]",
            "cmp w9, #47",
            "b.ne open_error",
            "ldr x9, =loads",
            "ldr w10, [x9]",
            "add w10, w10, #1",
            "str w10, [x9]",
        ]
        # Callees may destroy all argument registers: the loader must preserve them.
        lines += [f"mov x{i}, #0" for i in range(9)]
        lines += [f"movi v{i}.16b, #0" for i in range(8)]
        lines += [
            "mov x0, #1",
            "ret",
            "open_error: b error",
            ".ltorg",
            '.section .dlsym,"ax"',
            "cmp x0, #1",
            "b.eq 1f",
            "b error",
            "1: ldr x0, =fake_attach",
            "ret",
            ".ltorg",
            '.section .resume,"ax"',
            "ldr d8, [sp]",
            "add sp, sp, #0x70",
            "b checked",
            '.section .publisher_resume,"ax"',
            "add sp, sp, #0x160",
            "b checked_shared",
            '.section .image_resume,"ax"',
            "ldr d14, [sp]",
            "add sp, sp, #0xa0",
            "b checked_shared",
            '.section .predictions_resume,"ax"',
            "add sp, sp, #0x1a0",
            "b checked_shared",
            '.section .metadata_resume,"ax"',
            "add sp, sp, #0xb0",
            "b checked_shared",
            '.section .transform_resume,"ax"',
            "add sp, sp, #0x50",
            "b checked_shared",
            '.section .mode_resume,"ax"',
            "ret",
            '.section .control,"aw",@nobits',
            ".skip 160",
            '.section .counters,"aw"',
            "checks: .word 0",
            "loads: .word 0",
            "shared_checks: .word 0",
            ".balign 8",
            "shared_callbacks: .quad " + ", ".join(["checked_shared"] * (len(SITES) - 6)),
        ]
        (folder / "test.S").write_text("\n".join(lines) + "\n")
        (folder / "control.cpp").write_text(
            '#include "tracking/native_abi.h"\n'
            'extern "C" unsigned check_loader_control(ct::tracker::Control *state) {\n'
            "    if (state->loader_started != 1 || state->worker_exited) return 0;\n"
            "    state->worker_exited = 1;\n"
            "    if (state->loader_started != 1) return 0;\n"
            "    state->worker_exited = 0;\n"
            "    return 1;\n"
            "}\n"
        )
        subprocess.run(
            [
                "clang++",
                "--target=aarch64-linux-gnu",
                "-std=c++17",
                "-O2",
                "-ffreestanding",
                "-fno-exceptions",
                "-fno-unwind-tables",
                "-fno-asynchronous-unwind-tables",
                "-I" + str(ROOT / "src"),
                "-c",
                str(folder / "control.cpp"),
                "-o",
                str(folder / "control.o"),
            ],
            check=True,
        )
        layout = (
            "SECTIONS { .start 0x400000 : { *(.start) }\n"
            ".check 0x410000 : { *control.o(.text*) }\n"
            ".counters 0x500000 : { *(.counters) }\n"
            ".dlsym 0xc48360 : { *(.dlsym) }\n"
            ".dlopen 0xc48440 : { *(.dlopen) }\n"
            ".resume 0xeafc54 : { *(.resume) }\n"
            ".publisher_resume 0xeb3884 : { *(.publisher_resume) }\n"
            ".image_resume 0x2637684 : { *(.image_resume) }\n"
            ".predictions_resume 0xeb4284 : { *(.predictions_resume) }\n"
            ".metadata_resume 0xec2274 : { *(.metadata_resume) }\n"
            ".transform_resume 0x25e6a74 : { *(.transform_resume) }\n"
            f".mode_resume {MODE_SITE + 4:#x} : {{ *(.mode_resume) }}\n"
            f".text {CAVE:#x} : {{ *(.text) }}\n"
            f".control {CONTROL:#x} (NOLOAD) : {{ *(.control) }} }}\n"
            f"control = {CONTROL:#x};\n"
            + "".join(f"resume_{i} = {site + 4:#x};\n" for i, site in enumerate(SITES))
            + f"resume_mode = {MODE_SITE + 4:#x};\n"
            f"resume_config = {CONFIG_SITE + 4:#x};\n"
            "duplicate_config = 0xf2dd70;\npublish_config = 0xf2d974;\n"
        )
        (folder / "test.ld").write_text(layout)
        subprocess.run(
            [
                "clang",
                "--target=aarch64-linux-gnu",
                "-c",
                str(folder / "test.S"),
                "-o",
                str(folder / "test.o"),
            ],
            check=True,
        )
        subprocess.run(
            [
                "ld.lld",
                "-e",
                "_start",
                "-T",
                str(folder / "test.ld"),
                str(folder / "test.o"),
                str(folder / "control.o"),
                str(built / "loader.o"),
                "-o",
                str(folder / "test"),
            ],
            check=True,
        )
        subprocess.run(["qemu-aarch64-static", str(folder / "test")], check=True, timeout=5)
    return {
        "arm64_loader_executed": True,
        "native_binary_executed": False,
        "checks": [
            "unarmed original call",
            "dlopen/dlsym/attach",
            "already loaded call",
            "x0..x8 preservation",
            "full q0..q7 preservation",
            "two stack arguments",
            "single load",
            "loader and worker state use independent ABI fields",
            "private-only mode override",
            "unchanged stock mode",
            "inactive mode pass-through",
            "all shared-pose and image callbacks",
            "six image arguments",
            "unarmed shared callbacks",
        ],
        "loader_sha256": manifest["loader_sha256"],
    }


if __name__ == "__main__":
    print(json.dumps(check(ROOT / "build/native-tracker")))
