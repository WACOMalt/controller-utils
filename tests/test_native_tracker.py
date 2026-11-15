import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import build_native_tracker as builder


class AdapterBranchTest(unittest.TestCase):
    def test_arm64_branch_boundaries(self):
        self.assertEqual(builder.branch(0x1000, 0xFFC).hex(), "ffffff17")
        self.assertEqual(builder.branch(0, (1 << 27) - 4).hex(), "ffffff15")
        self.assertEqual(builder.branch(1 << 27, 0).hex(), "00000016")
        for source, target in ((0, 1), (0, 1 << 27), (1 << 27, -4)):
            with self.subTest(source=source, target=target), self.assertRaises(ValueError):
                builder.branch(source, target)


class ConfigRouteTests(unittest.TestCase):
    def test_cached_connections_republish_only_the_gamepad_config(self):
        route = (
            builder.source([0] * len(builder.SITES))
            .split(".global route_config\n", 1)[1]
            .split("library_path:", 1)[0]
        )
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / "route.S").write_text(
                """
.text
.global test_route
test_route:
sub sp, sp, #0x430
stp x20, x26, [sp, #0x100]
stp x25, x28, [sp, #0x110]
str x30, [sp, #0x120]
mov w28, w1
mov w25, w3
str w2, [sp, #0x3f4]
cmp w0, #1
b route_config
"""
                + route
                + """
resume_config: mov w0, #0
b finish
duplicate_config: mov w0, #1
b finish
publish_config:
add x9, sp, #0xb0
cmp x20, x9
b.ne invalid
add x9, sp, #0x98
cmp x26, x9
b.ne invalid
mov w0, #2
b finish
invalid: mov w0, #3
finish:
ldp x20, x26, [sp, #0x100]
ldp x25, x28, [sp, #0x110]
ldr x30, [sp, #0x120]
add sp, sp, #0x430
ret
.section .note.GNU-stack,"",@progbits
"""
            )
            (base / "test.c").write_text("""
extern int test_route(int, int, int, int);
static void finish(int status) {
    register long result __asm__("x0") = status;
    register long number __asm__("x8") = 93;
    __asm__ volatile("svc #0" : "+r"(result) : "r"(number) : "memory");
    __builtin_unreachable();
}
void _start(void) {
    for (int found=0; found<2; ++found)
    for (int id=0; id<64; ++id)
    for (int connected=0; connected<2; ++connected)
    for (int empty=0; empty<2; ++empty)
        if (test_route(found,id,connected,empty) !=
            (!found ? 0 : id==16 && connected && !empty ? 2 : 1)) finish(1);
    finish(0);
}
""")
            subprocess.run(
                [
                    "clang",
                    "--target=aarch64-linux-gnu",
                    "-fuse-ld=lld",
                    "-ffreestanding",
                    "-nostdlib",
                    "-static",
                    "-Wl,-e,_start",
                    str(base / "route.S"),
                    str(base / "test.c"),
                    "-o",
                    str(base / "test"),
                ],
                check=True,
            )
            subprocess.run(["qemu-aarch64-static", str(base / "test")], check=True, timeout=5)
