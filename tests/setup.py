#!/usr/bin/env python3
"""Exercise first boot, disk selection, real install, reboot and password login."""
import os
import hashlib
import json
import shutil
import struct
import time
from pathlib import Path
from guest import ROOT, TEST, Guest, memory, symbol_address, wait_until

iso_mode=os.environ.get('LUNA_SETUP_ISO')=='1'
source=TEST/('installer.iso' if iso_mode else 'installer.img'); target=TEST/'target.img'; guard=TEST/'guard.img'
source_index=2 if iso_mode else 0
target_index=0 if iso_mode else 1
guard_index=1 if iso_mode else 2
shutil.copyfile(ROOT/'dist'/('LunaOS.iso' if iso_mode else 'LunaOS.img'),source)
for path in [target,guard]:
    with path.open('wb') as f: f.truncate(128*1024*1024)
    with path.open('r+b') as f:
        f.write((b'OLD PARTITIONS '+path.name.encode()).ljust(512,b'!'))
        f.seek(-512,2);f.write(b'OLD BACKUP GPT'.ljust(512,b'!'))

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
source_before=sha(source); guard_before=sha(guard); target_before=sha(target)
extra=['-drive',f'file={target},format=raw,if=ide,index={target_index}',
       '-drive',f'file={guard},format=raw,if=ide,index={guard_index}']

def number(g,name): return int.from_bytes(memory(g,symbol_address(name),4),'little',signed=True)
def page(g,p,timeout=30): wait_until(lambda:number(g,'setup_page')==p,timeout)
def move(g,x,y):
    g.qcommand('input-send-event',{'events':[
        {'type':'abs','data':{'axis':'x','value':(x*32767+1022)//1023}},
        {'type':'abs','data':{'axis':'y','value':(y*32767+766)//767}}]})
    time.sleep(.04)
def click(g,x,y):
    move(g,x,y)
    for down in [True,False]:
        g.qcommand('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
        time.sleep(.07)
def type_text(g,s):
    for c in s: g.key({' ':'spc','-':'minus','_':'underscore'}.get(c,c))

g=Guest(source,extra_args=extra,setup=True,cdrom=iso_mode)
try:
    page(g,0);g.screenshot('setup-welcome')
    g.key('esc')
    wait_until(lambda:memory(g,symbol_address('setup_active'),1)==b'\0')
finally: g.close()
assert sha(source)==source_before and sha(target)==target_before and sha(guard)==guard_before
print('PASS: Try LunaOS leaves all disks unchanged',flush=True)
g=Guest(source,extra_args=extra,setup=True,cdrom=iso_mode)
try:
    page(g,0);g.key('ret');page(g,1)
    click(g,700,308) # Portuguese system language
    click(g,300,410) # US keyboard
    g.screenshot('setup-language')
    g.key('ret');page(g,2)
    wait_until(lambda:memory(g,symbol_address('detected'),1)==b'\x01')
    g.screenshot('setup-hardware')
    g.key('ret');page(g,3);g.screenshot('setup-network')
    g.key('ret');page(g,4)
    click(g,700,305) # Portugal
    g.screenshot('setup-region');g.key('ret');page(g,5)
    type_text(g,'trialuser');g.key('tab');type_text(g,'trialpassword81');g.key('tab');type_text(g,'trialpassword81')
    g.screenshot('setup-account');g.key('ret');page(g,6)
    click(g,300,279+source_index*65+21) # Source disk must remain unavailable
    assert number(g,'selected_disk')==-1
    g.key('ret');page(g,6)
    assert sha(source)==source_before and sha(target)==target_before and sha(guard)==guard_before
    click(g,300,279+target_index*65+23);assert number(g,'selected_disk')==target_index
    g.screenshot('setup-disks');g.key('ret');page(g,7)
    wrong_token=f'apagar {source_index}';type_text(g,wrong_token);g.key('ret');page(g,7)
    assert sha(target)==target_before
    for _ in wrong_token:g.key('backspace')
    type_text(g,f'apagar {target_index}');g.screenshot('setup-confirm')
    g.key('ret');page(g,8)
    assert memory(g,symbol_address('password'),65)==bytes(65)
    page(g,9,600);g.screenshot('setup-complete')
    assert sha(source)==source_before and sha(guard)==guard_before
    print('PASS: only selected target changed, copy verified and boot committed',flush=True)
    # QEMU -no-reboot/-no-shutdown pauses on guest reset instead of replacing the VM.
    def reset_requested():
        return g.p.poll() is not None or g.qcommand('query-status')['return']['status']=='shutdown'
    wait_until(reset_requested,15)
finally:g.close()

# Boot only the installed target: source media is completely absent.
g=Guest(target,setup=True)
try:
    page(g,10)
    g.key('meta_l');assert number(g,'active_app')==-1
    g.serial.send(b'whoami\n');time.sleep(.1)
    assert number(g,'active_app')==-1
    g.screenshot('setup-login')
    type_text(g,'wrongpass81');g.key('ret')
    wait_until(lambda:memory(g,symbol_address('login_busy'),1)==b'\x01')
    wait_until(lambda:memory(g,symbol_address('login_busy'),1)==b'\0',30)
    assert memory(g,symbol_address('setup_active'),1)==b'\x01'
    type_text(g,'trialpassword81');g.key('ret')
    wait_until(lambda:memory(g,symbol_address('setup_active'),1)==b'\0',30)
    g.command('whoami','trialuser')
    g.command('echo $HOME','/home/trialuser')
    g.command('pwd','/home/trialuser')
    credentials=g.command('cat /etc/luna-user','algorithm=pbkdf2-sha256')
    config={}
    for line in credentials.splitlines():
        if '=' in line:
            k,v=line.split('=',1);config[k]=v
    assert config['username']=='trialuser'
    expected=hashlib.pbkdf2_hmac('sha256',b'trialpassword81',bytes.fromhex(config['salt']),int(config['rounds'])).hex()
    assert config['hash']==expected and 'trialpassword81' not in credentials
    g.command('cat /etc/region','country=PT')
    g.command('cat /etc/luna-settings','language=1')
    g.command('cat /etc/setup-complete','LunaOS setup v1')
finally:g.close()
assert sha(source)==source_before and sha(guard)==guard_before
report={'result':'PASS','image_sha256':sha(ROOT/'dist'/('LunaOS.iso' if iso_mode else 'LunaOS.img')),
        'verified':['first-boot wizard','live mode without disk writes','language/keyboard/region selection',
                    'source disk protected','incorrect erase token refused','only selected disk formatted',
                    'sector read-back verification and boot commit','automatic reboot requested',
                    'installed target boots without source media','password gate and wrong-password rejection',
                    'PBKDF2 hash independently verified with Python','username/home/settings persisted'],
        'limits':['legacy IDE/ATA only','Wi-Fi radio port pending; no scan or association',
                  'included Linux sources are not executable drivers','regional timezone stored; clock remains UTC',
                  'quick format is not secure erase','no disk encryption']}
(ROOT/'dist'/('setup-iso-report.json' if iso_mode else 'setup-report.json')).write_text(json.dumps(report,indent=2)+'\n')
print('PASS: setup, disk installation, independent boot, password login and persistent settings')
