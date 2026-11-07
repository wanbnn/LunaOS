"""Deterministic FAT32 EFI system partition, using only Python's standard library."""
import struct

CORE_BYTES=32*1024*1024
ESP_BYTES=34*1024*1024
IMAGE_BYTES=CORE_BYTES+ESP_BYTES
ESP_SECTOR=CORE_BYTES//512


def make_esp(core,loader):
    assert len(core)==CORE_BYTES
    total=ESP_BYTES//512
    reserved=32
    fat_sectors=1
    while True:
        clusters=total-reserved-2*fat_sectors
        needed=((clusters+2)*4+511)//512
        if needed==fat_sectors:break
        fat_sectors=needed
    assert clusters>=65525
    image=bytearray(ESP_BYTES)
    image[:3]=b'\xeb\x58\x90';image[3:11]=b'LUNAOS  '
    struct.pack_into('<HBHBHHBHHHII',image,11,512,1,reserved,2,0,0,0xf8,0,32,64,ESP_SECTOR,total)
    struct.pack_into('<IHHIHH',image,36,fat_sectors,0,0,2,1,6)
    image[64]=0x80;image[66]=0x29
    struct.pack_into('<I',image,67,0x4c554546)
    image[71:82]=b'LUNA EFI   ';image[82:90]=b'FAT32   ';image[510:512]=b'\x55\xaa'
    struct.pack_into('<I',image,512,0x41615252)
    struct.pack_into('<I',image,512+484,0x61417272)
    struct.pack_into('<II',image,512+488,0xffffffff,0xffffffff)
    struct.pack_into('<I',image,512+508,0xaa550000)
    image[6*512:7*512]=image[:512];image[7*512:8*512]=image[512:1024]
    fat=bytearray(fat_sectors*512)
    struct.pack_into('<II',fat,0,0x0ffffff8,0x0fffffff)
    next_cluster=2
    data_sector=reserved+2*fat_sectors
    def allocate(data):
        nonlocal next_cluster
        count=max(1,(len(data)+511)//512);first=next_cluster
        if first+count>clusters+2:raise ValueError('EFI partition full')
        for c in range(first,first+count):
            struct.pack_into('<I',fat,c*4,c+1 if c+1<first+count else 0x0fffffff)
        at=(data_sector+first-2)*512
        image[at:at+len(data)]=data
        next_cluster+=count
        return first
    def entry(name,cluster,size=0,directory=False):
        e=bytearray(32);e[:11]=name;e[11]=0x10 if directory else 0x20
        struct.pack_into('<H',e,20,cluster>>16);struct.pack_into('<H',e,26,cluster&65535)
        struct.pack_into('<I',e,28,size)
        # 2024-01-01, valid FAT creation/access/write dates.
        for off in (16,18,24):struct.pack_into('<H',e,off,0x5821)
        return e
    root=allocate(bytes(512));efi=allocate(bytes(512));boot=allocate(bytes(512))
    binary=allocate(loader);payload=allocate(core)
    def directory(cluster,entries):
        at=(data_sector+cluster-2)*512
        raw=b''.join(entries);image[at:at+len(raw)]=raw
    label=entry(b'LUNA EFI   ',0);label[11]=8
    directory(root,[label,entry(b'EFI        ',efi,directory=True),entry(b'LUNACOREBIN',payload,len(core))])
    # LUNACORE.BIN is 8.3: eight-character base followed by BIN.
    directory(efi,[entry(b'.          ',efi,directory=True),entry(b'..         ',root,directory=True),entry(b'BOOT       ',boot,directory=True)])
    directory(boot,[entry(b'.          ',boot,directory=True),entry(b'..         ',efi,directory=True),entry(b'BOOTX64 EFI',binary,len(loader))])
    image[reserved*512:(reserved+fat_sectors)*512]=fat
    image[(reserved+fat_sectors)*512:(reserved+2*fat_sectors)*512]=fat
    return bytes(image)
