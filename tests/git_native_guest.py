#!/usr/bin/env python3
"""Execute the pinned upstream Git ELF32 inside LunaOS, never on the host."""
import http.server
import hashlib
import os
import shutil
import threading
from guest import Guest, ROOT, TEST
class LoggedGuest(Guest):
    def command(self, *args, **kwargs):
        response = super().command(*args, **kwargs)
        print(response, flush=True)
        if args[0].startswith('git '):
            assert 'fatal:' not in response and 'error:' not in response, response
        return response
# The default image must already contain /bin/git. No download or host relay.
shutil.copyfile(ROOT/'dist/LunaOS.img',TEST/'git-native-disk.img')
g=None
try:
    g=LoggedGuest(TEST/'git-native-disk.img',ram=int(os.environ.get('LUNA_GIT_TEST_RAM','128')))
    g.command('git --version','git version 2.56.1',timeout=45)
    g.command('echo $?','\n0\n')
    g.command('git init --template= -b main /home/luna/repo','Initialized empty Git repository',timeout=45)
    g.command('git -C /home/luna/repo config user.name Luna')
    g.command('git -C /home/luna/repo config user.email luna@example.invalid')
    g.command('write /home/luna/repo/hello.txt Hello-from-native-Git')
    g.command('git -C /home/luna/repo add hello.txt',timeout=45)
    g.command('echo $?','\n0\n')
    g.command('git -C /home/luna/repo commit -m initial',timeout=45)
    g.command('echo $?','\n0\n')
    g.command('git -C /home/luna/repo log --oneline','initial',timeout=45)
    payload=b'Hello-from-native-Git\n'
    oid=hashlib.sha1(b'blob '+str(len(payload)).encode()+b'\0'+payload).hexdigest()
    g.command('git -C /home/luna/repo hash-object hello.txt',oid)
    g.command('git -C /home/luna/repo tag v1')
    g.command('git -C /home/luna/repo switch -c feature','Switched to a new branch')
    g.command('write /home/luna/repo/feature.txt Native-branch')
    g.command('git -C /home/luna/repo add feature.txt')
    g.command('echo $?','\n0\n')
    g.command('git -C /home/luna/repo commit -m feature',timeout=45)
    g.command('echo $?','\n0\n')
    g.command('git -C /home/luna/repo switch main','Switched to branch')
    g.command('git -C /home/luna/repo merge --ff-only feature',timeout=45)
    g.command('echo $?','\n0\n')
    g.command('git -C /home/luna/repo fsck --full',timeout=45)
    g.command('echo $?','\n0\n')
    g.command('git clone /home/luna/repo /home/luna/copy','done.',timeout=45)
    g.command('echo $?','\n0\n')
    g.command('git -C /home/luna/copy log --oneline','feature')
    if os.environ.get('LUNA_GIT_TEST_TRANSPORT','1') != '0':
        g.command('git clone --bare /home/luna/repo /home/luna/remote.git','done.',timeout=45)
        g.command('git -C /home/luna/copy config user.name Luna')
        g.command('git -C /home/luna/copy config user.email luna@example.invalid')
        g.command('git -C /home/luna/copy remote set-url origin /home/luna/remote.git')
        g.command('write /home/luna/copy/pushed.txt Native-push')
        g.command('git -C /home/luna/copy add pushed.txt')
        g.command('git -C /home/luna/copy commit -m pushed',timeout=45)
        g.command('echo $?','\n0\n')
        g.command('git -C /home/luna/copy push origin main',timeout=60)
        g.command('echo $?','\n0\n')
        g.command('git -C /home/luna/repo remote add origin /home/luna/remote.git')
        g.command('git -C /home/luna/repo fetch origin',timeout=60)
        g.command('echo $?','\n0\n')
        g.command('git -C /home/luna/repo pull --ff-only origin main',timeout=60)
        g.command('echo $?','\n0\n')
        g.command('cat /home/luna/repo/pushed.txt','Native-push')
    g.command('git -C /home/luna/repo status --porcelain',timeout=45)
    g.command('echo $?','\n0\n')
    # Test signal mask/handler delivery plus private fork heap and pipe+dup2+
    # execve stdout, using a native C executable compiled against this musl.
    smoke=(ROOT/'build/git-native/smoke.elf').read_bytes()
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200);self.send_header('Content-Length',str(len(smoke)))
            self.end_headers();self.wfile.write(smoke)
        def log_message(self,*args): pass
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    try:
        g.command(f'wget http://10.0.2.2:{server.server_address[1]}/smoke /tmp/musl-smoke','Saved')
        g.command('/tmp/musl-smoke','LUNA MUSL FORK SIGNAL EXEC OK',timeout=45)
        g.command('echo $?','\n0\n')
    finally:
        server.shutdown()
    g.command('sync','snapshot committed',timeout=45)
    g.close();g=None
    g=LoggedGuest(TEST/'git-native-disk.img',network=False,ram=int(os.environ.get('LUNA_GIT_TEST_RAM','128')))
    g.command('git --version','git version 2.56.1')
    g.command('git -C /home/luna/repo log --oneline','feature')
    g.command('git -C /home/luna/repo fsck --full',timeout=45)
    g.command('echo $?','\n0\n')
    print('PASS: bundled upstream Git native workflows, interoperability hash, signals/fork/exec and reboot',flush=True)
finally:
    if g:g.close()

