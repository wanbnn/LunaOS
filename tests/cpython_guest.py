"""Check CPython and Python 3.12+ language features inside a LunaOS guest."""
import shutil

from guest import ROOT, TEST, Guest


def main():
    disk = TEST / "cpython.img"
    shutil.copyfile(ROOT / "dist/LunaOS.img", disk)
    guest = Guest(disk, network=False, ram=128)
    try:
        guest.command("python --version", "Python 3.13.9")
        guest.command("python3 -c 'import json, sys; print(sys.implementation.name, json.dumps(42))'",
                      "cpython 42\n", timeout=30)
        guest.command("python3.13 -c 'import sys; sys.exit(7)'", timeout=30)
        guest.command("echo $?", "7\n")
        guest.command("mkdir project")
        guest.command("echo 'def identity[T](x: T) -> T: return x' > project/peer.py")
        guest.command("echo 'from peer import identity' > project/main.py")
        guest.command("echo 'match identity(42):' >> project/main.py")
        guest.command('echo \'    case 42: print("modern Python syntax")\' >> project/main.py')
        guest.command("python project/main.py", "modern Python syntax\n", timeout=30)
        guest.command('python -c \'from pathlib import Path; print(Path("/home/luna/project/peer.py").is_file())\'',
                      "True\n", timeout=30)
        guest.command("python -m venv .venv", "Created Python environment")
        guest.command("venv activate .venv", "Activated")
        guest.command("echo 'answer = 61' > .venv/lib/localonly.py")
        guest.command("python -c 'import localonly; print(localonly.answer)'", "61\n", timeout=30)
        guest.command("deactivate", "deactivated")
        guest.command("python -c 'import localonly'", "ModuleNotFoundError", timeout=30)
        guest.command("python -m pip --version", "LunaOS pip")
    finally:
        guest.close()


if __name__ == "__main__":
    main()
