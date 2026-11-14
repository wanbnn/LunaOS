# Git upstream nativo no LunaOS

O build padrao inclui **Git 2.56.1** em `/bin/git` e **Dash 0.5.12** em
`/bin/sh`. Sao executaveis ELF32 i386 em ring 3, com libc musl adaptada a ABI
do LunaOS. O lunaSH os encontra pelo PATH. Nao ha reimplementacao da CLI Git
nem servico no host calculando commits, objetos ou historico.

**Porte experimental: HTTPS/SSH ainda nao estao implementados.** A existencia
do executavel e os testes locais nao encerram a matriz completa de
`tests/git-full-acceptance.md`.

## Uso na nova imagem

```
git --version
git init -b main /home/luna/projeto
git -C /home/luna/projeto config user.name "Seu Nome"
git -C /home/luna/projeto config user.email voce@example.com
write /home/luna/projeto/README.md Primeiro commit
git -C /home/luna/projeto add README.md
git -C /home/luna/projeto commit -m inicial
git -C /home/luna/projeto log --oneline
```

E necessario iniciar a imagem que contem este porte. Uma imagem anterior nao
passa a ter `/bin/git` apenas por reconhecer executaveis no PATH.

Os binarios e os auxiliares de transporte local sao montados como arquivos
imutaveis a partir de payloads comprimidos. Nao ocupam os slots gravaveis do
LunaFS e sao remontados apos restaurar snapshots. Repositorios e configuracao
do usuario continuam no LunaFS gravavel. `sync` salva o estado para reboot.

A configuracao inicial `/etc/gitconfig` define `maintenance.auto=false` para
evitar picos de processos em background no perfil com pouca RAM. E uma
configuracao normal do Git, editavel pelo usuario; nao altera o codigo upstream.
A imagem fornece um diretorio de templates vazio, sem hooks de exemplo.

## Build e validacao

```
git submodule update --init --recursive
make -j4 all
make test-elf32 test-lunafs2 test-pmm test-heap
python3 tests/git_native_guest.py
```

Dependencias do host: GCC/binutils com suporte i386, Python 3, autoconf,
automake, m4 e as dependencias usuais da imagem LunaOS. Geradores do Dash
sao executados no host; os aplicativos resultantes executam no guest.

- `make git-native-build`: `build/git-native/git.elf`, sysroot e Dash nativos.
- `build/git-native/sh.elf`: shell POSIX upstream.
- `build/git-native/smoke.elf`: teste de libc, sinais, fork/exec e stdio.
- `make test-git-native-guest`: build da imagem e teste QEMU.
- `make git-upstream-host-test`: baseline Linux separado, sem comprovar guest.

Git e Dash sao fixados respectivamente em
`03b41523c576f26ad34085378717a0c1b287470f` e
`4bbf8721a3ac6401ced6a0454956801f6ba37256`. O build usa copias isoladas em
`build/git-native`; nao modifica a libc usada pelo kernel nem o codigo do
submodulo Git. Flags upstream desabilitam Rust, pthreads, mmap, curl,
OpenSSL, expat, gettext, Perl/Tcl/Python e iconv nesta configuracao.
O fallback oficial NO_MMAP usa pread; nao ha mmap simulado.

Os testes QEMU verificam Git real pelo lunaSH, identidades, commits, branches,
merge fast-forward, tags, hash de blob comparado com SHA-1 calculado pelo
fixture, fsck e persistencia depois de reboot. O teste C separado verifica
mascara e entrega de sinais, heap privado depois de fork, pipe/dup2, execve
com Git, redirecionamento para /dev/null e status de terminacao por sinal.
Push, fetch e pull locais com packfiles foram testados com 128 MiB.
Para repetir apenas os fluxos offline pequenos com 64 MiB, use
`LUNA_GIT_TEST_RAM=64 LUNA_GIT_TEST_TRANSPORT=0 python3 tests/git_native_guest.py`.
O perfil completo usa 128 MiB por padrao.

## Limites conhecidos

- HTTPS/SSH nao estao portados: faltam libcurl/remote-https e um cliente SSH
  com verificacao de host keys. Nao ha bypass de TLS nem proxy obrigatorio.
- Sem pthreads, TLS de threads, interpretador ELF dinamico ou job control
  completo. Dash e usado para comandos nao interativos.
- VM de programa/heap de 8 MiB, heap limitado a 4 MiB, stack de 128 KiB e
  oito descritores gerais mais stdin/stdout/stderr por processo.
- Os offsets de arquivos regulares ainda sao copiados, nao compartilhados,
  por fork/dup. Pipes compartilham buffers e contadores de endpoints.
- LunaFS nao oferece symlinks/hardlinks POSIX, enforcement integral de
  permissoes, identidade de arquivo aberto apos unlink/rename nem locking
  fcntl completo. Metadados de tempo tem resolucao de segundos.
- Auxiliares e extensoes externos nao incluidos (SSH, GPG, LFS, editores,
  ferramentas de merge) continuam exigindo seus proprios portes.
- Grandes repositorios, falhas de disco/rede, interrupcoes e todos os fluxos
  avancados da matriz completa ainda precisam de validacao especifica.

As licencas e os caminhos do codigo-fonte ficam em `/etc/licenses`.
