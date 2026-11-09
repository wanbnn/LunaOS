"""Exercise the embedded Python runtime through the lunaSH entry points."""
import shutil

from guest import ROOT, TEST, Guest


def main():
    disk = TEST / "python.img"
    shutil.copyfile(ROOT / "dist/LunaOS.img", disk)
    guest = Guest(disk, network=False)
    try:
        guest.command("python --version", "MicroPython 1.29.0 on LunaOS")
        guest.command("python -c 'print(sum(n*n for n in range(5)), 2**64, 2.5/2)'",
                      "30 18446744073709551616 1.25")
        guest.command("python -c 'import sys; print(sys.platform, sys.argv[1])' hello",
                      "lunaos hello")
        guest.command("python -c 'print(1/0)'", "ZeroDivisionError")
        guest.command("echo $?", "1\n")
        guest.command("python -c 'import sys; sys.exit(0)' && echo clean", "clean")
        guest.command("python -c 'import sys; sys.exit(2)'")
        guest.command("echo $?", "2\n")
        guest.command("python -c 'while True: pass'", "Python: execution timed out")
        guest.command("python -c 'import gc; gc.collect(); print(42)'", "42\n")
        guest.command("python -c 'a=[]; a.append(a); print(a)'", "maximum recursion depth exceeded")
        guest.command("dev new scanner py", "Created py source")
        guest.command("dev run /home/luna/scanner.py", "Hello from LunaOS")
        guest.command("echo 'value = 42' > sibling.py")
        guest.command("echo 'import sibling; print(sibling.value)' > main.py")
        guest.command("python main.py", "42\n")
        guest.command("python missing.py", "OSError")
        guest.command("python -m venv .venv", "Created Python environment")
        guest.command("venv create alternate", "Created Python environment")
        guest.command("echo 'value = 52' > .venv/lib/project_only.py")
        guest.command("venv activate .venv", "Activated /home/luna/.venv")
        guest.command("python -c 'import project_only; print(project_only.value)'", "52\n")
        guest.command("venv activate alternate", "Activated /home/luna/alternate")
        guest.command("python -c 'import project_only'", "ImportError")
        guest.command("deactivate", "Python environment deactivated")
        guest.command("python -c 'import project_only'", "ImportError")
        guest.command("venv activate .venv/lib", "environment not found")
        guest.command("python -m venv /etc/unsafe", "choose a path within your home")
    finally:
        guest.close()


if __name__ == "__main__":
    main()
