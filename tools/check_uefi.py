"""Validate the real FAT32 chain, PE32+ machine and both boot catalog entries."""
import struct
from pathlib import Path
from efifat import CORE_BYTES, ESP_BYTES
raw = Path('dist/LunaOS.img').read_bytes()
assert len(raw) == CORE_BYTES + ESP_BYTES
entry = raw[462:478]
assert entry[4] == 0xef
start, sectors = struct.unpack_from('<II', entry, 8)
assert start * 512 == CORE_BYTES and sectors * 512 == ESP_BYTES
fat = raw[CORE_BYTES:]
assert fat[82:90] == b'FAT32   ' and fat[510:512] == b'\x55\xaa'
reserved = struct.unpack_from('<H', fat, 14)[0]
fat_sectors = struct.unpack_from('<I', fat, 36)[0]
cluster_bytes = fat[13] * 512
data_sector = reserved + fat[16] * fat_sectors

def chain(cluster):
    seen = set()
    while cluster < 0xffffff8:
        assert 2 <= cluster and cluster not in seen
        seen.add(cluster)
        offset = data_sector * 512 + (cluster-2)*cluster_bytes
        yield fat[offset:offset+cluster_bytes]
        cluster = struct.unpack_from('<I', fat, reserved*512+cluster*4)[0] & 0xfffffff

def file(directory, name):
    data = b''.join(chain(directory))
    for pos in range(0, len(data), 32):
        e = data[pos:pos+32]
        if e[:11] == name:
            return struct.unpack_from('<H',e,20)[0]<<16 | struct.unpack_from('<H',e,26)[0], struct.unpack_from('<I',e,28)[0]
    raise AssertionError(name)
root = struct.unpack_from('<I', fat, 44)[0]
efi,_ = file(root,b'EFI        ')
boot,_ = file(efi,b'BOOT       ')
loader,size = file(boot,b'BOOTX64 EFI')
pe = b''.join(chain(loader))[:size]
assert pe == Path('build/BOOTX64.EFI').read_bytes() and pe[:2] == b'MZ'
offset = struct.unpack_from('<I',pe,60)[0]
assert pe[offset:offset+4] == b'PE\0\0'
assert struct.unpack_from('<H',pe,offset+4)[0] == 0x8664
assert struct.unpack_from('<H',pe,offset+24)[0] == 0x20b
assert struct.unpack_from('<H',pe,offset+24+68)[0] == 10
core,size = file(root,b'LUNACOREBIN')
assert size == CORE_BYTES and b''.join(chain(core))[:size] == raw[:CORE_BYTES]
iso = Path('dist/LunaOS.iso').read_bytes()
catalog_lba = struct.unpack_from('<I',iso,17*2048+71)[0]
catalog = iso[catalog_lba*2048:(catalog_lba+1)*2048]
assert catalog[64:68] == bytes([0x91,0xef,1,0]) and catalog[96:98] == bytes([0x88,0])
efi_lba = struct.unpack_from('<I',catalog,104)[0]
assert iso[efi_lba*2048:efi_lba*2048+ESP_BYTES] == fat
print('PASS: FAT32 EFI partition / BOOTX64.EFI PE32+ / core payload / EFI El Torito catalog')
