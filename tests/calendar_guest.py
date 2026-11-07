#!/usr/bin/env python3
"""Offline calendar/persistence and real UDP SNTP integration through QEMU."""
import shutil
import socket
import struct
import threading
import time
from guest import ROOT, TEST, Guest, memory, symbol_address, wait_until

def move(g,x,y):
    if memory(g,symbol_address("absolute_pointer"),1)==b"\x01":
        g.qcommand("input-send-event",{"events":[{"type":"abs","data":{"axis":"x","value":(x*32767+1022)//1023}},{"type":"abs","data":{"axis":"y","value":(y*32767+766)//767}}]})
        wait_until(lambda: abs(int.from_bytes(memory(g,symbol_address("mouse_x"),4),"little")-x)<=1 and abs(int.from_bytes(memory(g,symbol_address("mouse_y"),4),"little")-y)<=1)
        return
    for _ in range(60):
        ox=int.from_bytes(memory(g,symbol_address("mouse_x"),4),"little",signed=True);oy=int.from_bytes(memory(g,symbol_address("mouse_y"),4),"little",signed=True)
        if (ox,oy)==(x,y):return
        dx=max(-70,min(70,x-ox));dy=max(-70,min(70,y-oy))
        g.qcommand("input-send-event",{"events":[{"type":"rel","data":{"axis":"x","value":dx}},{"type":"rel","data":{"axis":"y","value":dy}}]})
        time.sleep(.06)
    raise AssertionError("Pointer move timed out")
def click(g,x,y):
    move(g,x,y)
    for down in (True,False):
        g.qcommand("input-send-event",{"events":[{"type":"btn","data":{"button":"left","down":down}}]});time.sleep(.1)

path=TEST/"calendar.img";shutil.copyfile(ROOT/"dist/LunaOS.img",path)
g=Guest(path,network=False,ram=256)
try:
    g.command("date zone -03:00","UTC-03:00")
    g.command("date set 2032-02-29 12:34:56","2032-02-29 12:34:56")
    g.command('js "new Date().toISOString().slice(0,16)"', "2032-02-29T15:34")
    g.command("date set 2100-02-29 12:00:00","usage:")
    g.command("date","2032-02-29")
    click(g,955,24);g.screenshot("calendar")
    click(g,840,382);time.sleep(.5);g.screenshot("date-time-settings")
    # Configure the date through real mouse/keyboard events in Settings.
    click(g,425,307)
    for _ in range(10):g.key("backspace")
    for c in "2032-03-01":g.key("minus" if c=="-" else c)
    click(g,425,502)
    g.command("date","2032-03-01")
finally:g.close()
g=Guest(path,network=False,ram=256)
try:
    text=g.command("date","2032-03-01");assert "UTC-03:00" in text and "(manual)" in text,text
    g.command("date sync","Waiting for network")
    assert "2032-03-01" in g.command("date")
finally:g.close()
sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);sock.bind(("0.0.0.0",0));port=sock.getsockname()[1];sock.settimeout(.5)
stop=threading.Event();received=threading.Event()
def serve():
    while not stop.is_set():
        try:p,addr=sock.recvfrom(512)
        except socket.timeout:continue
        if len(p)!=48:continue
        received.set();reply=bytearray(48);reply[0]=0x24;reply[1]=2;reply[24:32]=p[40:48]
        # Fixed 2040 UTC date exercises the NTP era rollover in 2036.
        stamp=struct.pack("!II",(2208988800+2208988800)&0xffffffff,0)
        reply[32:40]=stamp;reply[40:48]=stamp;sock.sendto(reply,addr)
thread=threading.Thread(target=serve,daemon=True);thread.start()
g=Guest(path,ram=256)
try:
    g.command(f"date server 10.0.2.2 {port}","10.0.2.2")
    g.command("date sync")
    assert received.wait(10),"No NTP datagram received"
    wait_until(lambda:"Synchronized via NTP" in g.command("date"),20)
    g.command("date zone +00:00","2040-01-01")
    print("PASS: offline calendar, manual settings, reboot persistence and UDP NTP (era 2040)",flush=True)
finally:
    g.close();stop.set();thread.join(2);sock.close()
