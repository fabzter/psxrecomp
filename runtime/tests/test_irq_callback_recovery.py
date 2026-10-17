"""Run production dirty-RAM recovery regressions without a game/general build.

Artifacts are preserved. Pass --out-dir to name an existing empty directory
(it must be empty so the preserved artifacts stay identifiable); with no
--out-dir a fresh temporary directory is created instead, which is what ctest
registration uses so the test is repeatable.
"""
import argparse
import os
import tempfile
from pathlib import Path

from fixture_link import build_and_run


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--out-dir", type=Path,
                        help="existing empty directory to preserve artifacts in; "
                             "defaults to a fresh temporary directory")
    args = parser.parse_args()
    # fixture_link shells out to `nm`; it lives beside the GNU compiler, which may
    # not be on PATH (e.g. a clang-configured CMake tree passing a full gcc path).
    cc_dir = str(Path(args.cc).resolve().parent)
    os.environ["PATH"] = cc_dir + os.pathsep + os.environ.get("PATH", "")
    if args.out_dir is None:
        out_dir = Path(tempfile.mkdtemp(prefix="irq-callback-recovery-"))
    else:
        if not args.out_dir.is_dir() or any(args.out_dir.iterdir()):
            parser.error("--out-dir must be an existing empty directory (artifacts are preserved)")
        out_dir = args.out_dir
    print(f"artifacts: {out_dir}", flush=True)
    here = Path(__file__).resolve().parent
    for opt in ("-O0", "-O2"):
        build_and_run(args.cc, here, here.parent, opt, out_dir,
                      ["dirty_ram_interp.c"], "test_irq_callback_recovery.c",
                      defines=("PSX_HAS_GAME_DISPATCH", "PSX_NO_DEBUG_TOOLS"))
        print(f"PASS: IRQ callback recovery {opt}", flush=True)
