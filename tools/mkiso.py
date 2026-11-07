#!/usr/bin/env python3
"""Build a small ISO9660/El Torito CD using Luna's own HDD-emulation loader."""
import hashlib
import json
import struct
from pathlib import Path
from efifat import CORE_BYTES, ESP_BYTES, IMAGE_BYTES, ESP_SECTOR

BLOCK=2048
ROOT=21
CATALOG=22
IMAGE=64

def both16(n):return struct.pack('<H',n)+struct.pack('>H',n)
def both32(n):return struct.pack('<I',n)+struct.pack('>I',n)
def directory(name,extent,size,flags=0):
    length=33+len(name)+(not len(name)%2)
    record=bytearray(length);record[0]=length
    record[2:10]=both32(extent);record[10:18]=both32(size)
    record[18:25]=bytes([124,1,1,0,0,0,0]);record[25]=flags
    record[28:32]=both16(1);record[32]=len(name);record[33:33+len(name)]=name
    return record

raw=Path('dist/LunaOS.img').read_bytes()
assert len(raw)==IMAGE_BYTES and raw[510:512]==b'\x55\xaa'
embedded=bytearray(raw)
struct.pack_into('<I',embedded,432,IMAGE)
struct.pack_into('<H',embedded,444,0xcd01)
struct.pack_into('<I',embedded,440,int.from_bytes(hashlib.sha256(raw).digest()[:4],'little') or 1)
# Valid HDD-emulation CHS geometry: 32 cylinders, 64 heads, 32 sectors/track.

blocks=IMAGE+len(raw)//BLOCK
pvd=bytearray(BLOCK);pvd[:7]=b'\x01CD001\x01';pvd[8:40]=b'LUNAOS'.ljust(32,b' ')
pvd[40:72]=b'LUNAOS'.ljust(32,b' ');pvd[80:88]=both32(blocks)
pvd[120:124]=both16(1);pvd[124:128]=both16(1);pvd[128:132]=both16(BLOCK)
pvd[132:140]=both32(10);struct.pack_into('<I',pvd,140,19);struct.pack_into('>I',pvd,148,20)
pvd[156:190]=directory(b'\0',ROOT,BLOCK,2);pvd[881]=1
boot=bytearray(BLOCK);boot[:7]=b'\x00CD001\x01';boot[7:39]=b'EL TORITO SPECIFICATION'.ljust(32,b'\0')
struct.pack_into('<I',boot,71,CATALOG)
terminator=b'\xffCD001\x01'+bytes(BLOCK-7)
le=struct.pack('<BBIH',1,0,ROOT,1)+b'\0\0'
be=struct.pack('>BBIH',1,0,ROOT,1)+b'\0\0'
root=directory(b'\0',ROOT,BLOCK,2)+directory(b'\1',ROOT,BLOCK,2)
root+=directory(b'BOOT.CAT;1',CATALOG,BLOCK)+directory(b'LUNAOS.IMG;1',IMAGE,len(raw))
catalog=bytearray(BLOCK);catalog[0]=1;catalog[4:28]=b'LunaOS own BIOS loader'.ljust(24,b' ')
catalog[30:32]=b'\x55\xaa'
struct.pack_into('<H',catalog,28,(-sum(struct.unpack('<16H',catalog[:32])))&0xffff)
struct.pack_into('<BBHBBHI',catalog,32,0x88,4,0,0xda,0,1,IMAGE)
# EFI platform section, FAT32 volume inside the same raw disk image.
catalog[64:68]=bytes([0x91,0xef,1,0])
struct.pack_into('<BBHBBHI',catalog,96,0x88,0,0,0,0,1,IMAGE+CORE_BYTES//BLOCK)
iso=Path('dist/LunaOS.iso')
with iso.open('wb') as f:
    f.truncate(blocks*BLOCK)
    for sector,data in [(16,pvd),(17,boot),(18,terminator),(19,le),(20,be),(ROOT,root),(CATALOG,catalog),(IMAGE,embedded)]:
        f.seek(sector*BLOCK);f.write(data)
manifest={'name':'LunaOS','format':'ISO9660 / El Torito BIOS + UEFI x64','bytes':iso.stat().st_size,
          'sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),'image_lba':IMAGE,
          'base_img_sha256':hashlib.sha256(raw).hexdigest(),'embedded_img_sha256':hashlib.sha256(embedded).hexdigest(),
          'bootloader':'Luna native BIOS / HDD EDD + El Torito CHS','uefi':True,'uefi_arch':'x86_64','secure_boot_signed':False,'esp_lba':IMAGE+CORE_BYTES//BLOCK}
Path('dist/iso-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
Path('dist/SHA256SUMS').write_text(manifest['base_img_sha256']+'  LunaOS.img\n'+manifest['sha256']+'  LunaOS.iso\n')
print(f'{iso}: {iso.stat().st_size/1024/1024:.3f} MiB / own bootloader / read-only CD source')
