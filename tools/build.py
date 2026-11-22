import subprocess
import sys
from pathlib import Path

from extract_iso import build_lock, extract_iso, parse_iso_arg, select_iso


def main():
    filename = parse_iso_arg("bota a iso safada em rom/ e deixa o recompilador gozar")

    root = Path(__file__).resolve().parent.parent
    build = root / "build"
    try:
        with build_lock(build):
            image = select_iso(root / "rom", filename)
            extract_iso(image, build / "converted", reuse=True)
            subprocess.run(
                ["make", "-C", "runtime", "GAME_ELF=../build/converted/jogo.elf", f"PYTHON={sys.executable}"],
                cwd=root,
                check=True,
            )
    except subprocess.CalledProcessError as error:
        return error.returncode
    except (OSError, ValueError) as error:
        raise SystemExit(f"deu ruim {error}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
