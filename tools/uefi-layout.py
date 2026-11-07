import subprocess
from pathlib import Path
symbols = dict(line.split()[::-2] for line in subprocess.check_output(['nm', 'build/kernel.elf'], text=True).splitlines() if len(line.split()) == 3)
names = {'LUNA_LOW_END':'__bss_low_end', 'LUNA_HIGH_START':'__bss_high_start', 'LUNA_BSS_END':'__bss_end'}
Path('build/uefi_layout.h').write_text(''.join(f'#define {key} 0x{symbols[value]}ull\n' for key,value in names.items()))
