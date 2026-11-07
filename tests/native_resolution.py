#!/usr/bin/env python3
"""Verify native desktop pixels, geometry and pointer bounds at every video mode."""
import hashlib
import json
import shutil
import struct
import time
from PIL import Image
from guest import ROOT, TEST, Guest, memory, symbol_address, wait_until

shutil.copyfile(ROOT/'dist/LunaOS.img',TEST/'native.img')
g=Guest(TEST/'native.img')
WIN=struct.Struct('<9iIBBBx')
def integer(name): return int.from_bytes(memory(g,symbol_address(name),4),'little',signed=True)
def window(app=4):return WIN.unpack(memory(g,symbol_address('windows')+WIN.size*app,WIN.size))
def dimensions():return integer('screen_width'),integer('screen_height')
def move(x,y):
    w,h=dimensions()
    g.qcommand('input-send-event',{'events':[
        {'type':'abs','data':{'axis':'x','value':(x*32767+w-2)//(w-1)}},
        {'type':'abs','data':{'axis':'y','value':(y*32767+h-2)//(h-1)}}]})
    wait_until(lambda:abs(integer('mouse_x')-x)<=1 and abs(integer('mouse_y')-y)<=1)
def click(x,y):
    move(x,y)
    for down in [True,False]:
        g.qcommand('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
        time.sleep(.08)
def screenshot(name):
    wait_until(lambda:memory(g,symbol_address('ui_dirty'),1)==b'\0',30)
    path=TEST/(name+'.ppm');g.qcommand('screendump',{'filename':str(path)})
    im=Image.open(path).convert('RGB');assert im.size==dimensions()
    im.save(ROOT/'dist'/(name+'.png'))
    return im

def title_metrics(im):
    w=window();x,y=w[1],w[2]
    title=im.crop((x+137,y+7,x+337,y+31))
    mask=Image.new('L',title.size)
    mask.putdata([255 if min(title.getpixel((x,y)))>230 else 0 for y in range(title.height) for x in range(title.width)])
    bbox=mask.getbbox();assert bbox
    close=im.crop((x+14,y+7,x+34,y+27))
    red=Image.new('L',close.size)
    red.putdata([255 if p[0]>220 and p[1]<160 and p[2]<180 else 0 for y in range(close.height) for x in range(close.width) for p in [close.getpixel((x,y))]])
    b=red.getbbox();assert b
    return (bbox[2]-bbox[0],bbox[3]-bbox[1]),(b[2]-b[0],b[3]-b[1])

def set_mode(index):
    w=window();x,y=w[1]+20,w[2]+46
    click(x+250,y+105)
    wait_until(lambda:memory(g,symbol_address('resolution_open'),1)==b'\x01')
    click(x+250,y+146+36*index+15)
    wait_until(lambda:integer('display_mode')==index)
    wait_until(lambda:memory(g,symbol_address('ui_dirty'),1)==b'\0',30)

try:
    g.open_app('settings');w=window();x,y=w[1]+20,w[2]+46
    click(x+65,y+62+2*47+18)
    baseline=title_metrics(screenshot('native-1024'))
    metrics=[]
    for i,(width,height) in enumerate([(1024,768),(1280,720),(1360,768),(1600,900),(1920,1080)]):
        if i:set_mode(i)
        assert dimensions()==(width,height)
        move(width-1,height-1)
        im=screenshot(f'native-{width}')
        current=title_metrics(im)
        for pair,expected in zip(current,baseline):
            assert all(abs(a-b)<=1 for a,b in zip(pair,expected)),(width,current,baseline)
        for x,y in [(0,0),(width-1,0),(width-1,height-1),(0,height-1)]:move(x,y)
        click(width-174,26)
        wait_until(lambda:memory(g,symbol_address('controls_open'),1)==b'\x01')
        g.key('esc')
        if i>=3:
            w=window();click(w[1]+76,w[2]+17)
            wait_until(lambda:window()[12]==1)
            w=window();assert (w[1],w[2],w[3],w[4])==(8,54,width-16,height-162)
            screenshot(f'native-maximized-{width}')
            click(w[1]+76,w[2]+17);wait_until(lambda:window()[12]==0)
        metrics.append({'mode':f'{width}x{height}','title_pixels':current[0],'close_button_pixels':current[1]})
        print('PASS: native rendering / constant glyph and circle size / corners / panel:',width,height,flush=True)
    # Alternate two oversized clients through the shared full-resolution cache.
    w=window();click(w[1]+76,w[2]+17);wait_until(lambda:window()[12]==1)
    g.open_app('tasks');w=window(5)
    click(w[1]+76,w[2]+17);wait_until(lambda:window(5)[12]==1)
    screenshot('native-tasks-maximized-1920')
    g.open_app('settings');screenshot('native-settings-refocused-1920')
    # A mode change while maximized must constrain the window to the new native workarea.
    set_mode(1);w=window();assert w[3]==1264 and w[4]==558
    click(w[1]+76,w[2]+17);wait_until(lambda:window()[12]==0)
    # Persist 1600x900 and verify a new boot uses an actual native canvas.
    set_mode(3);w=window();x,y=w[1]+20,w[2]+46
    click(x+85,y+w[4]-66-30)
    g.command('cat /etc/luna-settings','output_mode=3')
    g.close();g=Guest(TEST/'native.img')
    assert dimensions()==(1600,900)
    g.open_app('settings');screenshot('native-persisted-1600')
    g.command('setup')
    wait_until(lambda:memory(g,symbol_address('setup_active'),1)==b'\x01')
    im=screenshot('native-setup-1600')
    assert min(im.getpixel((150,400)))<230 and min(im.getpixel((1150,400)))>220
    g.key('esc')
    (ROOT/'dist/native-resolution-report.json').write_text(json.dumps({
        'result':'PASS','image_sha256':hashlib.sha256((ROOT/'dist/LunaOS.img').read_bytes()).hexdigest(),
        'verified':['native canvas equals scanout dimensions','no whole-desktop scaling','constant-size native text and round controls','mouse reaches native corners','right-anchored controls','native maximized window geometry','native resolution restored after boot'],
        'pixel_metrics':metrics,'interface_scale_percent':100,'ram_mib':64},indent=2)+'\n')
finally:g.close()
