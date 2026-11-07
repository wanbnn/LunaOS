#!/usr/bin/env python3
"""Independent PCI/MMIO/DMA checks; does not link LunaOS driver code."""
from pathlib import Path
import os
import struct
import subprocess
ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'build/wifi-qtest'
WORK.mkdir(parents=True,exist_ok=True)
env=os.environ.copy()
env['LD_LIBRARY_PATH']=str(ROOT/'.tools/usr/lib/x86_64-linux-gnu')
log=(WORK/'qemu.log').open('w')
q=subprocess.Popen([str(ROOT/'.tools/qemu-storage-build/qemu-system-i386'),
 '-L',str(ROOT/'.tools/qemu-storage-src/pc-bios'),'-machine','pc','-S',
 '-nodefaults','-display','none','-qtest','stdio','-device',
 'lab-rtw8822be,addr=04.0,firmware='+str(ROOT/'assets/firmware/rtw88/rtw8822b_fw.bin')],
 stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True,env=env)
def cmd(s):
    q.stdin.write(s+'\n'); q.stdin.flush()
    line=q.stdout.readline().strip()
    assert line.startswith('OK'),(s,line)
    return int(line.split()[1],0) if len(line.split())>1 else None

def cfg(reg,value=None):
    cmd(f'outl 0xcf8 {0x80002000+reg}')
    return cmd('inl 0xcfc') if value is None else cmd(f'outl 0xcfc {value}')
BASE=0xe0000000
def rd(r):return cmd(f'readl {BASE+r}')
def wr(r,v):cmd(f'writel {BASE+r} {v}')
def write(a,b):cmd(f'write {a} {len(b)} 0x{b.hex()}')
try:
    assert cfg(0)==0xb82210ec
    cfg(0x18,BASE); cfg(4,2)
    assert rd(0xf0)==0x08001000
    # Read-only hardware completion and efuse encoding independent of driver.
    wr(0x30,0); assert rd(0x30)&0x800000ff==0x8000002f
    wr(0xc90,(0x18<<20)|36); assert rd(0x2860)==36
    cmd(f'writeb {BASE+5} 1'); assert cmd(f'readb {BASE+5}')&3==0
    # Descriptor checksum independently constructed from upstream TX format.
    payload=b'\x5a'*32
    desc=bytearray(48)
    struct.pack_into('<I',desc,0,len(payload)|(48<<16)|(1<<26)|(1<<31))
    struct.pack_into('<I',desc,4,16<<8)
    checksum=0
    for off in range(0,32,2):checksum^=struct.unpack_from('<H',desc,off)[0]
    struct.pack_into('<H',desc,28,checksum)
    bd=struct.pack('<HHIHHI',48,0x8001,0x110000,len(payload),0,0x120000)
    write(0x100000,bd);write(0x110000,desc);write(0x120000,payload)
    wr(0x308,0x100000)
    cmd(f'writeb {BASE+0x383} 16')
    assert rd(0x204)&0x8000==0,'DMA must be blocked while BME disabled'
    cfg(4,6)
    # Bad descriptor cannot complete even with BME enabled.
    desc[28]^=1;write(0x110000,desc)
    cmd(f'writeb {BASE+0x383} 16');assert rd(0x204)&0x8000==0
    desc[28]^=1;write(0x110000,desc)
    cmd(f'writeb {BASE+0x383} 16');assert rd(0x204)&0x8000
    cmd(f'writew {BASE+0x204} 32768');assert rd(0x204)&0x8000==0
    # Wrong transferred firmware bytes must produce IDDMA checksum failure.
    wr(0x1200,0x18780030);wr(0x1204,0x200000);wr(0x1208,0xa0000020)
    assert rd(0x1208)&(1<<27)
    cmd(f'writew {BASE+0x80} 0x4000');cmd(f'writeb {BASE+3} 4')
    assert rd(0x80)&0xffff!=0xc078,'partial/corrupt firmware must never boot'
    print('PASS: PCI identity, efuse, RF, BME, descriptor checksum, W1C, corrupt firmware rejection')
finally:
    q.terminate();q.wait(timeout=5);log.close()
