import argparse
import fcntl
import json
import os
import shutil
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path
from tempfile import TemporaryDirectory

from elf_reader import read_header


PRESENTATION_IMAGES = {"ICON0.PNG", "PIC0.PNG", "PIC1.PNG"}
DISC_DIRECTORY = ".disc"
SOURCE_FILENAME = ".source.json"


class IsoHelpFormatter(argparse.HelpFormatter):
    def _format_usage(self, usage, actions, groups, prefix=None):
        return super()._format_usage(usage, actions, groups, "uso: ")


def parse_iso_arg(description):
    parser = argparse.ArgumentParser(
        description=description,
        formatter_class=IsoHelpFormatter,
        add_help=False,
    )
    arguments = parser.add_argument_group("argumentos")
    arguments.add_argument("-h", "--help", action="help", help="mostra essa ajuda e sai")
    arguments.add_argument("iso", nargs="?", help="nome da iso se tiver mais de uma em rom/")
    args, extra = parser.parse_known_args()
    if extra:
        parser.print_usage(sys.stderr)
        parser.exit(2, f"não entendi esses argumentos {' '.join(extra)}\n")
    return args.iso


@contextmanager
def build_lock(build):
    build.mkdir(parents=True, exist_ok=True)
    with (build / ".psbridge.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


def select_iso(rom, filename):
    images = sorted(path for path in rom.iterdir() if path.suffix.lower() == ".iso" and path.is_file())
    if filename is not None:
        images = [image for image in images if image.name == filename]
    if not images:
        if filename is not None:
            raise ValueError(f"não achei {filename} em rom/")
        raise ValueError("não achei nenhuma iso em rom/")
    if len(images) > 1:
        raise ValueError("tem mais de uma iso em rom/, passa o nome da que tu quer")
    return images[0]


def is_game_elf(path):
    if not path.is_file():
        return False
    with path.open("rb") as source:
        if source.read(4) != b"\x7fELF":
            return False
        source.seek(0)
        try:
            read_header(source.read())
        except ValueError:
            return False
    return True


def write_game_elf(sysdir, output):
    eboot = sysdir / "EBOOT.BIN"
    boot = sysdir / "BOOT.BIN"
    if eboot.is_file():
        with eboot.open("rb") as source:
            magic = source.read(4)
        if magic == b"~PSP":
            decrypt = shutil.which("pspdecrypt")
            if decrypt is None:
                raise ValueError("cadê o pspdecrypt no PATH pra abrir o EBOOT.BIN?")
            result = subprocess.run([decrypt, "-o", output, eboot], capture_output=True)
            if result.returncode != 0 or not is_game_elf(output):
                raise ValueError("deu ruim descriptografando o EBOOT.BIN dessa iso safada")
            return
        if is_game_elf(eboot):
            shutil.copyfile(eboot, output)
            return

    if is_game_elf(boot):
        shutil.copyfile(boot, output)
        return
    raise ValueError("não achei executável que preste em PSP_GAME/SYSDIR/")


def organize_extraction(extracted):
    game = extracted / "PSP_GAME"
    sysdir = game / "SYSDIR"
    if not sysdir.is_dir():
        raise ValueError("cadê a pasta PSP_GAME/SYSDIR/ nessa iso?")

    images = [entry for entry in game.iterdir() if entry.is_file() and entry.name.upper() in PRESENTATION_IMAGES]
    assets = [entry for entry in game.iterdir() if entry not in images]
    elf = extracted / "jogo.elf"
    if any(entry.name == elf.name for entry in assets):
        raise ValueError(f"tem mais de um caminho pro {elf.name} nessa iso")
    for name in [elf.name, *(entry.name for entry in assets)]:
        target = extracted / name
        if target.exists() or target.is_symlink():
            raise ValueError(f"tem mais de um caminho pra {name} nessa iso")

    with TemporaryDirectory(dir=extracted) as staging:
        output = Path(staging) / elf.name
        write_game_elf(sysdir, output)
        output.rename(elf)

    for entry in assets:
        entry.rename(extracted / entry.name)
    for image in images:
        image.unlink()
    game.rmdir()


def iso_source(image):
    info = image.stat()
    return {"name": image.name, "size": info.st_size, "mtime_ns": info.st_mtime_ns, "inode": info.st_ino}


def create_disc_view(extracted, destination, image, source):
    disc = extracted / DISC_DIRECTORY
    disc.mkdir()
    game = disc / "PSP_GAME"
    game.mkdir()
    for entry in extracted.iterdir():
        if entry.name not in {DISC_DIRECTORY, SOURCE_FILENAME, "jogo.elf", "UMD_DATA.BIN"}:
            (game / entry.name).symlink_to(Path("../..") / entry.name, target_is_directory=entry.is_dir())
    (disc / "UMD_DATA.BIN").symlink_to("../UMD_DATA.BIN")
    (disc / "UMD.ISO").symlink_to(os.path.relpath(image, destination / DISC_DIRECTORY))
    with (extracted / SOURCE_FILENAME).open("x") as stamp:
        json.dump(source, stamp)
        stamp.write("\n")


def extract_iso(image, destination, reuse=False):
    if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
        raise ValueError("build/converted/ tem que ser uma pasta")

    source = iso_source(image)
    stamp = destination / SOURCE_FILENAME
    populated = destination.is_dir() and any(destination.iterdir())
    if populated:
        if not reuse or not stamp.is_file():
            raise ValueError("já tem coisa em build/converted/")
        if json.loads(stamp.read_text()) == source and is_game_elf(destination / "jogo.elf"):
            return

    with TemporaryDirectory(dir=destination.parent) as staging:
        staging = Path(staging)
        extracted = staging / "extracted"
        extracted.mkdir()
        subprocess.run(
            ["7z", "x", "-tiso", "-bso0", "-bsp0", f"-o{extracted}", "--", image],
            check=True,
        )
        organize_extraction(extracted)
        create_disc_view(extracted, destination, image, source)
        if iso_source(image) != source:
            raise ValueError("a iso mudou no meio da extração")

        previous = staging / "previous"
        if populated:
            destination.rename(previous)
        try:
            extracted.rename(destination)
        except OSError:
            if populated:
                previous.rename(destination)
            raise

    print(f"extraí a iso safada {image.name} em build/converted/", flush=True)


def main():
    filename = parse_iso_arg("pega a iso safada em rom/ e deixa o jogo.elf pronto em build/converted/")

    root = Path(__file__).resolve().parent.parent
    try:
        with build_lock(root / "build"):
            extract_iso(select_iso(root / "rom", filename), root / "build" / "converted")
    except subprocess.CalledProcessError:
        raise SystemExit("deu ruim extraindo a iso")
    except (OSError, ValueError) as error:
        raise SystemExit(f"deu ruim extraindo a iso, {error}")


if __name__ == "__main__":
    main()
