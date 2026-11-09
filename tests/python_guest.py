"""Exercise the embedded Python runtime through the lunaSH entry points."""
import shutil

from guest import ROOT, TEST, Guest


def main():
    disk = TEST / "python.img"
    shutil.copyfile(ROOT / "dist/LunaOS.img", disk)
    guest = Guest(disk, network=False)
    try:
        guest.command("micropython --version", "MicroPython 1.29.0 on LunaOS")
        guest.command("micropython -c 'print(sum(n*n for n in range(5)), 2**64, 2.5/2)'",
                      "30 18446744073709551616 1.25")
        guest.command("micropython -c 'import sys; print(sys.platform, sys.argv[1])' hello",
                      "lunaos hello")
        guest.command("micropython -c 'print(1/0)'", "ZeroDivisionError")
        guest.command("echo $?", "1\n")
        guest.command("micropython -c 'import sys; sys.exit(0)' && echo clean", "clean")
        guest.command("micropython -c 'import sys; sys.exit(2)'")
        guest.command("echo $?", "2\n")
        guest.command("micropython -c 'while True: pass'", "Python: execution timed out")
        guest.command("micropython -c 'import gc; gc.collect(); print(42)'", "42\n")
        guest.command("micropython -c 'a=[]; a.append(a); print(a)'", "maximum recursion depth exceeded")
        guest.command("dev new scanner py", "Created py source")
        guest.command("dev run /home/luna/scanner.py", "Hello from LunaOS")
        guest.command("echo 'value = 42' > sibling.py")
        guest.command("echo 'import sibling; print(sibling.value)' > main.py")
        guest.command("micropython main.py", "42\n")
        guest.command("micropython missing.py", "OSError")
        guest.command("micropython -m venv .venv", "Created Python environment")
        guest.command("venv create alternate", "Created Python environment")
        guest.command("echo 'value = 52' > .venv/lib/project_only.py")
        guest.command("venv activate .venv", "Activated /home/luna/.venv")
        guest.command("micropython -c 'import project_only; print(project_only.value)'", "52\n")
        guest.command("venv activate alternate", "Activated /home/luna/alternate")
        guest.command("micropython -c 'import project_only'", "ImportError")
        guest.command("deactivate", "Python environment deactivated")
        guest.command("micropython -c 'import project_only'", "ImportError")
        guest.command("venv activate .venv/lib", "environment not found")
        guest.command("micropython -m venv /etc/unsafe", "choose a path within your home")
    finally:
        guest.close()


if __name__ == "__main__":
    main()
