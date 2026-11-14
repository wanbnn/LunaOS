#!/usr/bin/env python3
"""Build an isolated musl sysroot for LunaOS, never modify the kernel's libc."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build/git-native'
SRC = BUILD / 'musl-src'
OBJ = BUILD / 'musl'
PORT = ROOT / 'sdk/musl-luna'
JOBS = os.environ.get('LUNA_BUILD_JOBS', '4')
def run(*cmd, cwd=ROOT, env=None):
    subprocess.run([str(x) for x in cmd], cwd=cwd, check=True, env=env)
def prepare():
    if not SRC.exists():
        shutil.copytree(ROOT / 'third_party/musl', SRC,
                        ignore=shutil.ignore_patterns('obj','lib','config.mak','*.o','*.a'))
    changed = False
    patches = {
        'syscall_arch.h':'arch/i386/syscall_arch.h',
        'pthread_arch.h':'arch/i386/pthread_arch.h',
        'restore.s':'src/signal/i386/restore.s',
        'syscall_cp.s':'src/thread/i386/syscall_cp.s',
        'clone.s':'src/thread/i386/clone.s',
        'set_thread_area.s':'src/thread/i386/__set_thread_area.s',
        'vfork.s':'src/process/i386/vfork.s',
        'unmapself.s':'src/thread/i386/__unmapself.s',
    }
    for name, relative in patches.items():
        target = SRC/relative
        if target.read_bytes() != (PORT/name).read_bytes():
            shutil.copyfile(PORT/name,target)
            changed = True
    # musl does not track architecture headers as dependencies of all objects.
    if changed and (OBJ/'config.mak').exists():
        run('make','clean',cwd=OBJ)
    OBJ.mkdir(parents=True, exist_ok=True)
    if not (OBJ/'config.mak').exists():
        run(SRC/'configure', '--target=i386', '--disable-shared',
            '--prefix='+str(BUILD/'sysroot'), 'CC=gcc -m32', 'AR=ar', 'RANLIB=ranlib',
            'CFLAGS=-Os -fno-pie -fno-pic -fno-stack-protector -mno-sse -mno-mmx -mfpmath=387', cwd=OBJ)
    run('make', '-j'+JOBS, 'AR=ar', 'RANLIB=ranlib', cwd=OBJ)
    run('make', 'install', 'AR=ar', 'RANLIB=ranlib', cwd=OBJ)
    includes = ['-nostdinc','-isystem',BUILD/'sysroot/include',
                '-isystem',subprocess.check_output(['gcc','-print-file-name=include'],text=True).strip(),
                '-I'+str(SRC/'src/include'),'-I'+str(SRC/'src/internal'),'-I'+str(SRC/'arch/i386'),
                '-I'+str(SRC/'arch/generic'),'-I'+str(OBJ/'obj/src/internal')]
    flags = ['-m32','-Os','-ffreestanding','-fno-builtin','-fno-pie','-fno-pic',
             '-fno-stack-protector','-fno-asynchronous-unwind-tables',
             '-mno-sse','-mno-mmx','-msoft-float','-D_GNU_SOURCE']
    for name in ('start','syscall'):
        run('gcc',*flags,*includes,'-c',PORT/(name+'.c'),'-o',BUILD/(name+'.o'))
    run('gcc',*flags,'-Isdk/guest','-c','sdk/guest/memory.c','-o',BUILD/'memory.o')
    run('gcc','-m32','-c',PORT/'crt0.S','-o',BUILD/'crt0.o')
def build_smoke():
    run(BUILD/'luna-cc','-Os','-fno-pie','-fno-stack-protector','-c',PORT/'smoke.c','-o',BUILD/'smoke.o')
    run('ld','-m','elf_i386','--gc-sections','--build-id=none','-T',ROOT/'sdk/guest/luna32.ld',
        '-o',BUILD/'smoke.elf',BUILD/'crt0.o',BUILD/'start.o',BUILD/'memory.o',BUILD/'syscall.o',
        BUILD/'smoke.o',BUILD/'sysroot/lib/libc.a',
        subprocess.check_output(['gcc','-m32','-print-libgcc-file-name'],text=True).strip())
def build_dash():
    upstream=ROOT/'third_party/dash'
    sha=subprocess.check_output(['git','-C',str(upstream),'rev-parse','HEAD'],text=True).strip()
    if sha!='4bbf8721a3ac6401ced6a0454956801f6ba37256':
        raise RuntimeError('Initialize pinned Dash: git submodule update --init --recursive')
    source=BUILD/'dash-src'
    if not source.exists():
        shutil.copytree(upstream,source,ignore=shutil.ignore_patterns('.git'))
    env=os.environ.copy()
    local=ROOT/'.tools/usr'
    if (local/'bin/autoconf').exists():
        for tool in ('aclocal','automake'):
            candidates=sorted((local/'bin').glob(tool+'-[0-9]*'))
            if candidates and not (local/'bin'/tool).exists():
                (local/'bin'/tool).symlink_to(candidates[-1].name)
        env['PATH']=str(local/'bin')+os.pathsep+env.get('PATH','')
        env['AC_MACRODIR']=str(local/'share/autoconf')
        cfg=BUILD/'autom4te.cfg'
        cfg.write_text((local/'share/autoconf/autom4te.cfg').read_text().replace('/usr/share/autoconf',str(local/'share/autoconf')))
        env['AUTOM4TE_CFG']=str(cfg)
        env['trailer_m4']=str(local/'share/autoconf/autoconf/trailer.m4')
        env['autom4te_perllibdir']=str(local/'share/autoconf')
        env['AUTOM4TE']=str(local/'bin/autom4te')
        env['M4']=str(local/'bin/m4')
        automake=sorted((local/'share').glob('automake-[0-9]*'))[-1]
        env['PERL5LIB']=str(automake)+os.pathsep+str(local/'share/autoconf')
        env['AUTOMAKE_LIBDIR']=str(automake)
        env['ACLOCAL_AUTOMAKE_DIR']=str(local/'share'/automake.name.replace('automake','aclocal'))
    if not (source/'configure').exists():
        run('sh','autogen.sh',cwd=source,env=env)
    obj=BUILD/'dash';obj.mkdir(exist_ok=True)
    if not (obj/'Makefile').exists():
        linker='-nostdlib -static -no-pie -Wl,--gc-sections,--build-id=none -Wl,-T,'+str(ROOT/'sdk/guest/luna32.ld')
        linker+=' '+ ' '.join(str(BUILD/(name+'.o')) for name in ('crt0','start','memory','syscall'))
        libs=str(BUILD/'sysroot/lib/libc.a')+' '+subprocess.check_output(['gcc','-m32','-print-libgcc-file-name'],text=True).strip()
        run(source/'configure','--host=i386-luna','--build=x86_64-linux-gnu','--without-libedit',
            'CC='+str(BUILD/'luna-cc'),'CC_FOR_BUILD=gcc',
            'CFLAGS=-Os -fno-pie -fno-pic -fno-stack-protector -ffunction-sections -fdata-sections',
            'LDFLAGS='+linker,'LIBS='+libs,cwd=obj,env=env)
    run('make','-j'+JOBS,cwd=obj,env=env)
    # Relink when the native ABI support changed (external objects in LDFLAGS).
    (obj/'src/dash').unlink(missing_ok=True)
    run('make','-j'+JOBS,cwd=obj,env=env)
    shutil.copyfile(obj/'src/dash',BUILD/'sh.elf')
def build_curl():
    """Build the LunaOS libcurl-compatible client used by the Git HTTP
    transport. The TLS policy lives in the kernel; this library owns HTTP."""
    cc = BUILD/'luna-cc'
    run(cc,'-Os','-fno-pie','-fno-pic','-fno-stack-protector',
        '-fno-asynchronous-unwind-tables','-mno-sse','-mno-mmx','-mfpmath=387',
        '-I'+str(ROOT/'sdk/curl-luna/include'),'-I'+str(ROOT/'third_party/zlib'),
        '-c',str(ROOT/'sdk/curl-luna/curl-luna.c'),'-o',str(BUILD/'curl-luna.o'))
    run('ar','rcs',str(BUILD/'libcurl-luna.a'),str(BUILD/'curl-luna.o'))
    config = BUILD/'curl-config'
    config.write_text(
        '#!/bin/sh\n'
        '# Reports the LunaOS HTTP client as a libcurl replacement.\n'
        'case "$1" in\n'
        '  --version) echo "7.66.0" ;;\n'
        '  --vernum) echo "074200" ;;\n'
        '  --cflags) echo "-I'+str(ROOT/'sdk/curl-luna/include')+'" ;;\n'
        '  --libs) echo "'+str(BUILD/'libcurl-luna.a')+'" ;;\n'
        '  --static) echo "'+str(BUILD/'libcurl-luna.a')+'" ;;\n'
        '  --protocols) echo "http https" ;;\n'
        '  --features) echo "SSL" ;;\n'
        '  *) echo "" ;;\n'
        'esac\n')
    config.chmod(0o755)
def build_git():
    upstream = ROOT/'third_party/git'
    sha = subprocess.check_output(['git','-C',str(upstream),'rev-parse','HEAD'],text=True).strip()
    run('git','-C',upstream,'diff','--exit-code','HEAD','--')
    if sha != '03b41523c576f26ad34085378717a0c1b287470f':
        raise RuntimeError('Unpinned Git source')
    target = BUILD/'git-src'
    if not (target/'.source-ready').exists():
        paths = subprocess.check_output(['git','-C',str(upstream),'ls-files','-z']).split(b'\0')
        for raw in paths:
            if not raw: continue
            relative = os.fsdecode(raw)
            dest = target/relative
            dest.parent.mkdir(parents=True,exist_ok=True)
            if (upstream/relative).is_dir():
                shutil.copytree(upstream/relative,dest,dirs_exist_ok=True,ignore=shutil.ignore_patterns('.git','*.o','*.a'))
            else:
                shutil.copy2(upstream/relative,dest)
        (target/'.source-ready').touch()
    cc = BUILD/'luna-cc'
    cc.write_text('#!/bin/sh\nexec gcc -m32 -nostdinc -isystem "'+str(BUILD/'sysroot/include')+'" -isystem "'+subprocess.check_output(['gcc','-print-file-name=include'],text=True).strip()+'" "$@"\n')
    cc.chmod(0o755)
    build_curl()
    config = {
        'CC':str(cc),'AR':'ar','RANLIB':'ranlib', 'uname_S':'LunaOS',
        'CFLAGS':'-I'+str(ROOT/'third_party/zlib')+' -Os -fno-pie -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -mfpmath=387 -ffunction-sections -fdata-sections',
        'LDFLAGS':'-nostdlib -static -no-pie -Wl,--gc-sections,--build-id=none -Wl,-T,'+str(ROOT/'sdk/guest/luna32.ld')+' '+str(BUILD/'crt0.o')+' '+str(BUILD/'start.o')+' '+str(BUILD/'memory.o')+' '+str(BUILD/'syscall.o'),
        'EXTLIBS':str(ROOT/'build/libz-luna.a')+' '+str(BUILD/'sysroot/lib/libc.a')+' '+subprocess.check_output(['gcc','-m32','-print-libgcc-file-name'],text=True).strip(),
        'CURL_CONFIG':str(BUILD/'curl-config'),
        'CURL_CFLAGS':'-I'+str(ROOT/'sdk/curl-luna/include'),
        'CURL_LDFLAGS':str(BUILD/'libcurl-luna.a'),
        'NO_RUST':'YesPlease','NO_GETTEXT':'YesPlease','NO_TCLTK':'YesPlease',
        'NO_PERL':'YesPlease','NO_PYTHON':'YesPlease',
        'NO_EXPAT':'YesPlease','NO_OPENSSL':'YesPlease','NO_ICONV':'YesPlease',
        'NO_PTHREADS':'YesPlease','NO_MMAP':'YesPlease','NO_UNIX_SOCKETS':'YesPlease',
        'NO_DYNLOAD':'YesPlease','NO_REGEX':'YesPlease',
        'prefix':'/usr','sysconfdir':'/etc','GIT_VERSION':'2.56.1', 'GIT_BUILT_FROM_COMMIT':sha,
    }
    (target/'git').unlink(missing_ok=True)
    (target/'git-remote-http').unlink(missing_ok=True)
    run('make','-j'+JOBS,*[k+'='+v for k,v in config.items()],'git','git-remote-http',cwd=target)
    shutil.copyfile(target/'git',BUILD/'git.elf')
    shutil.copyfile(target/'git-remote-http',BUILD/'git-remote-http.elf')
    build_smoke()
    build_dash()
if __name__ == '__main__':
    prepare()
    if '--git' in sys.argv: build_git()
