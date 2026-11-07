#!/usr/bin/env python3
"""RTL8822BE laboratory integration: real native driver, no Ethernet NIC."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'build/wifi-qemu'
WORK.mkdir(parents=True,exist_ok=True)
os.environ['LUNA_TEST_DIR']=str(WORK)
os.environ['LUNA_TEST_OFFLINE']='1'
os.environ['LUNA_QEMU']=str(ROOT/'.tools/qemu-storage-build/qemu-system-i386')
sys.path.insert(0,str(ROOT/'tests'))
from guest import Guest

def capture(path):
    # Explicit test APs, never compiled into production driver or image.
    packets=[]
    for channel,ssid in [(1,b'Laboratorio-2G'),(36,b'Laboratorio-5G')]:
        bssid=bytes([2,0,0,0,0,channel])
        frame=b'\x80\x00\x00\x00'+b'\xff'*6+bssid+bssid+b'\x00\x00'
        frame+=b'\x00'*8+struct.pack('<HH',100,1)
        frame+=bytes([0,len(ssid)])+ssid+b'\x01\x01\x82'+bytes([3,1,channel])
        packets.append(struct.pack('<IIII',0,0,len(frame),len(frame))+frame)
    path.write_bytes(struct.pack('<IHHIIII',0xa1b2c3d4,2,4,0,0,2304,105)+b''.join(packets))

pcap=WORK/'beacons.pcap'
capture(pcap)
fw=ROOT/'assets/firmware/rtw88/rtw8822b_fw.bin'
base=f'lab-rtw8822be,firmware={fw}'
report={'result':'RUNNING','scope':'partial RTL8822BE passive RX; no physical hardware or TX certification','cases':{}}
(WORK/'report.json').write_text(json.dumps(report,indent=2)+'\n')
for case,options in [('scan',f',capture={pcap}'),('no-air',''),('firmware-timeout',',stall-firmware=on')]:
    disk=WORK/(case+'.img')
    shutil.copyfile(ROOT/'dist/LunaOS.img',disk)
    g=None
    try:
        g=Guest(disk,network=False,extra_args=['-device',base+options])
        status=g.command('wifidiag')
        assert 'QEMU laboratory model; passive RX only' in status,status
        if case=='firmware-timeout':
            assert 'firmware upload/MCU boot' in status and 'error=110' in status,status
            g.command('wifi status','driver unavailable')
        else:
            assert 'passive RX ready' in status,status
            g.command('wifi status','connect=0')
            g.command('wifi scan','scan started')
            deadline=time.monotonic()+20
            while True:
                state=g.command('wifi status')
                if 'link=1' not in state: break
                assert time.monotonic()<deadline,state
                time.sleep(.3)
            results=g.command('wifi results')
            if case=='scan':
                assert 'networks=2' in results,results
                assert 'Laboratorio-2G channel=1' in results,results
                assert 'Laboratorio-5G channel=36' in results,results
            else:
                assert 'networks=0' in results,results
        report['cases'][case]='PASS'
        print('PASS:',case,flush=True)
    finally:
        if g:
            (WORK/(case+'.serial.log')).write_text(g.transcript)
            g.close()
report['result']='PASS'
report['kernel_sha256']=hashlib.sha256((ROOT/'build/kernel.bin').read_bytes()).hexdigest()
report['model_sha256']=hashlib.sha256((ROOT/'tests/fixtures/wifi/rtw8822be.c').read_bytes()).hexdigest()
(WORK/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: Wi-Fi native driver QEMU integration',flush=True)
