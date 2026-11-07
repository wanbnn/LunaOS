#!/usr/bin/env python3
"""Guest evidence for absolute mouse, display modes, Notes and System settings."""
import hashlib, json, shutil, struct, time
from guest import ROOT, TEST, Guest, memory, symbol_address, wait_until
shutil.copyfile(ROOT/'dist/LunaOS.img',TEST/'next.img')
g=Guest(TEST/'next.img')
def integer(name): return int.from_bytes(memory(g,symbol_address(name),4),'little',signed=True)
def move(x,y):
    w,h=integer('screen_width'),integer('screen_height')
    g.qcommand('input-send-event',{'events':[
        {'type':'abs','data':{'axis':'x','value':(x*32767+w-2)//(w-1)}},
        {'type':'abs','data':{'axis':'y','value':(y*32767+h-2)//(h-1)}}]})
    wait_until(lambda: abs(integer('mouse_x')-x)<=1 and abs(integer('mouse_y')-y)<=1)
def click(x,y):
    move(x,y)
    for down in [True,False]:
        g.qcommand('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
        time.sleep(.12)
def window(app): return struct.unpack('<9iIBBBx',memory(g,symbol_address('windows')+44*app,44))
def combo(*keys):
    g.qcommand('send-key',{'keys':[{'type':'qcode','data':k} for k in keys]});time.sleep(.2)
def typed(s):
    for c in s: g.key({' ':'spc','/':'slash','.':'dot','\n':'ret'}.get(c,c))
try:
    assert memory(g,symbol_address('absolute_pointer'),1)==b'\x01'
    for xy in [(0,0),(1023,0),(1023,767),(0,767),(512,384)]: move(*xy)
    print('PASS: absolute pointer reaches all four desktop corners',flush=True)
    g.open_app('settings');w=window(4);x,y=w[1]+20,w[2]+46
    click(x+65,y+62+2*47+18)
    click(x+250,y+105)
    assert memory(g,symbol_address('resolution_open'),1)==b'\x01'
    click(x+250,y+146+36+15)
    wait_until(lambda:integer('display_mode')==1)
    g.qcommand('screendump',{'filename':str(TEST/'1280.ppm')})
    from PIL import Image
    assert Image.open(TEST/'1280.ppm').size==(1280,720)
    click(x+85,y+w[4]-66-30)
    g.command('cat /etc/luna-settings','output_mode=1')
    g.close();g=Guest(TEST/'next.img')
    assert integer('display_mode')==1
    g.open_app('settings');w=window(4);x,y=w[1]+20,w[2]+46
    click(x+65,y+62+2*47+18)
    click(x+250,y+105);click(x+250,y+146+15)
    wait_until(lambda:integer('display_mode')==0)
    print('PASS: dropdown changes actual scanout to 1280x720 and back',flush=True)
    click(x+65,y+62+6*47+18)
    click(x+250,y+175)
    assert int.from_bytes(memory(g,symbol_address('prefs')+188,4),'little')==1
    click(x+250,y+105)
    click(x+85,y+w[4]-66-30)
    g.screenshot('settings-system')
    g.open_app('notes');wait_until(lambda:integer('active_app')==6)
    typed('hello')
    combo('ctrl','s')
    wait_until(lambda:memory(g,symbol_address('dirty'),1)==b'\0')
    g.screenshot('notes')
    result=g.command('cat /home/luna/note.md','hello')
    print('PASS: Notes is a separate app and saves via LunaFS',flush=True)
    g.command('cat /etc/luna-settings','developer=1')
    g.close()
    g=Guest(TEST/'next.img')
    assert int.from_bytes(memory(g,symbol_address('prefs')+184,4),'little')==1
    assert int.from_bytes(memory(g,symbol_address('prefs')+188,4),'little')==1
    g.command('cat /home/luna/note.md','hello')
    print('PASS: System language, developer preference and note persist after reboot',flush=True)
    (ROOT/'dist/desktop-next-report.json').write_text(json.dumps({
        'result':'PASS','image_sha256':hashlib.sha256((ROOT/'dist/LunaOS.img').read_bytes()).hexdigest(),
        'verified':['absolute pointer reaches four corners','resolution dropdown changes scanout 1280x720 and back','separate Notes app writes persistent file','System language and developer preferences persist'],
        'limits':['interface scale remains 100%','physical GPU mode enumeration pending','language translation partial','Wi-Fi and Bluetooth drivers pending','WebKit port pending','Notes has lexical highlighting, no type checker']},indent=2)+'\n')
finally: g.close()
