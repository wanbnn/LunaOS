#!/usr/bin/env python3
"""Verify the real System button and offline result on a disposable QEMU disk."""
import shutil
import struct
import time
from guest import ROOT, TEST, Guest, memory, symbol_address, wait_until


def click(g, x, y):
    for _ in range(60):
        ox = int.from_bytes(memory(g, symbol_address('mouse_x'), 4), 'little', signed=True)
        oy = int.from_bytes(memory(g, symbol_address('mouse_y'), 4), 'little', signed=True)
        if abs(ox - x) <= 1 and abs(oy - y) <= 1:
            break
        if memory(g, symbol_address('absolute_pointer'), 1) == b'\x01':
            events = [{'type': 'abs', 'data': {'axis': a, 'value': v}} for a, v in
                      [('x', (x * 32767 + 1022) // 1023), ('y', (y * 32767 + 766) // 767)]]
        else:
            events = [{'type': 'rel', 'data': {'axis': a, 'value': max(-70, min(70, v))}}
                      for a, v in [('x', x - ox), ('y', y - oy)]]
        g.qcommand('input-send-event', {'events': events})
        time.sleep(.06)
    else:
        raise AssertionError('pointer movement timed out')
    for down in (True, False):
        g.qcommand('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': down}}]})
        time.sleep(.12)


path = TEST / 'update-ui.img'
shutil.copyfile(ROOT / 'dist/LunaOS.img', path)
g = Guest(path, network=False, ram=256)
try:
    g.open_app('settings')
    window = struct.Struct('<9iIBBBx')
    w = window.unpack(memory(g, symbol_address('windows') + 4 * window.size, window.size))
    x, y = w[1] + 20, w[2] + 46
    click(g, x + 70, y + 62 + 6 * 47 + 18)
    click(g, x + 220, y + 332)
    wait_until(lambda: b'Connect to the internet' in memory(g, symbol_address('update_message'), 192))
    g.screenshot('system-updates-offline')
    g.command('ps', 'PID')
    print('PASS: System update button gives offline status; guest remains responsive')
finally:
    g.close()
