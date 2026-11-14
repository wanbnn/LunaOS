# Estado do porte upstream Git — 13/11/2026

**O porte completo ainda nao esta concluido. Nao existe `git.elf` executavel
no LunaOS.** Compilar o Git no Linux nao substitui essa evidencia.

## Analise de branches

- `main` local estava em `82448a7`, 164 commits atras de `origin/main`.
  Foi atualizada por fast-forward para `0fccc60`.
- `origin/feat/full-upstream-git-port` (`1d1a540`) e ancestral de
  `origin/main`: seu conteudo ja entrou pelo PR #8. Fazer merge dessa branch
  novamente nao acrescenta um executavel Git.
- `origin/feat/lunash-native-git-foundation` (`9b0565a`) contem um built-in
  parcial de init/config/hash-object. Nao atende ao contrato upstream e
  permanece separado, conforme a decisao documentada do projeto.
- `feat/git-upstream-luna-runtime` aponta para a mesma base da main, em
  outro worktree com alteracoes locais. Essas alteracoes foram preservadas.
  A correcao independente da dependencia de `memory.o` no Makefile foi
  reaproveitada na continuacao.
- A continuacao esta em `feat/git-native-runtime-completion`.
  As demais branches remotas tratam de browser, storage, wireless, motion e
  atualizacoes; nao contem uma implementacao alternativa completa do Git.

## Infraestrutura existente

| Area | Implementacao verificavel |
| --- | --- |
| Loader | ELF32 i386 ET_EXEC estatico, 8 MiB de VA segment-relative, arquivo ate 16 MiB, 16 segmentos, stack 16 KiB |
| Processo | ring 3, argv/envp limitados, spawn/wait e substituicao execve preservando PID/FDs |
| Heap | brk com paginas alocadas pelo PMM e limite 2 MiB por processo; malloc/calloc/realloc/free bootstrap |
| I/O | 8 descritores alem do console; arquivos, diretorios, pipes herdados com espera limitada a 30 s |
| Metadata | chmod e mtime persistentes no snapshot v3; ainda sem enforcement completo |
| Compressao | zlib upstream 1.4 compilada para i386 e executada no guest |
| Rede | transporte exclusivo TCP ou TLS verificado, compartilhado pelo kernel; nao e BSD sockets |

A ABI usa offsets relativos ao segmento, numeros proprios de syscall e
retornos negativos; nao e a ABI Linux. A musl que ja integra o kernel nao
fornece automaticamente uma libc para esses executaveis.

## Correcoes nesta continuacao

- `open(O_CREAT|O_EXCL)` insere o arquivo com preempcao desabilitada somente
  durante a operacao em RAM. Uma disputa retorna `-17` (`LUNA_EEXIST`), sem
  truncar o arquivo existente mesmo quando `O_TRUNC` tambem foi solicitado.
- `open` permite escrita sem exigir truncamento ou append. `lseek` permite
  reposicionar um arquivo gravavel; descritores de stream nao sao seekable.
- `fs_write_at` preserva prefixo/sufixo e preenche com zeros o intervalo
  entre o EOF e o offset. Uma escrita de zero bytes nao estende o arquivo.
- Arquivos pequenos usam substituicao do buffer; acima de 64 KiB a escrita
  reconstroi o snapshot por blocos de 16 KiB, preservando o snapshot anterior
  em falhas de staging. Esse caminho custa O(tamanho do snapshot) por escrita;
  ainda precisa de trabalho de desempenho para packfiles grandes.
- `O_APPEND` escolhe o EOF em cada chamada de escrita, independentemente de
  `lseek`. Isso nao atesta todas as garantias POSIX de append concorrente.
- `build/guest/memory.o` tem regra propria: o teste runtime pode ser compilado
  sem depender de ter compilado antes o teste de zlib.

- O baseline host e seu CI selecionam `NO_RUST=YesPlease`, opcao oficial do
  upstream. Sem isso, a receita falhava com `cargo: not found`.

## Verificacao reproduzivel

```
git submodule update --init third_party/git third_party/zlib
make test-lunafs2 test-elf32
make -j4 all luna-guest-runtime-smoke luna-guest-spawn-smoke \
    luna-guest-execve-smoke luna-guest-tcp-smoke luna-guest-metadata-check
make luna-guest-zlib-smoke
python3 tests/elf32_guest.py
make git-upstream-host-test
```

`tests/lunafs2.c` cobre overwrite binario, lacunas, overflow, zero-byte write,
metadata, persistencia e falha de snapshot durante patch de arquivo de 3 MiB,
com ASan/UBSan. `sdk/guest/runtime_smoke.c` exercita no guest o lock disputado
por outro processo, protecao contra truncamento, seek/overwrite/zero-fill e
append, alem do heap e pipes. O fixture QEMU tambem testa execve, zlib, TCP e
persistencia de arquivos/metadata apos reboot.

## Resultados desta verificacao

- `make test-lunafs2 test-elf32`: PASS com ASan/UBSan; o teste de filesystem
  foi repetido apos a revisao final de aritmetica de ponteiros, tambem PASS.
- Build padrao com Lexbor/litehtml: PASS, kernel 5.560.148 bytes,
  imagem 66 MiB e ISO 66,125 MiB.
- Cross-build dos executaveis SDK e zlib: PASS.
- `python3 tests/elf32_guest.py`: PASS, incluindo lock/seek/escrita/append,
  heap/IPC, execve, zlib, TCP e metadata apos reboot.
- `make git-upstream-host-test`: PASS com Git 2.56.1, init/add/commit,
  branch/merge/rebase/stash, clone/fetch/pull/push em remoto local, fsck/gc.
- `git diff --check` e sintaxe Python do fixture: PASS.

Esses resultados foram obtidos localmente; nao sao uma execucao de CI remoto
nem uma demonstracao do executavel Git dentro do LunaOS.

## Trabalho ainda necessario para aceitar o porte completo

1. Sysroot/libc nativos para compilar e ligar Git upstream e seus auxiliares,
   com errno, stdio, tempo, locale e demais APIs realmente utilizadas.
2. Descritores com compartilhamento de offsets entre processos, dup2 e
   redirecionamento de stdin/stdout/stderr; sinais e mecanismos de criacao
   de processos exigidos pelo Git e seu shell auxiliar.
3. VFS com symlinks, identidade de arquivos abertos apos rename/unlink,
   permissao, locking e garantias de durabilidade/concorrencia testadas.
4. Memoria/stack dimensionadas com o executavel real e perfis de consumo.
5. Cliente smart HTTP(S) com POST, streaming e autenticacao segura; SSH com
   host keys verificadas. O transporte TCP/TLS isolado nao implementa isso.
6. Binario upstream executado via lunaSH e toda a matriz offline/remota,
   interoperabilidade e falhas de `tests/git-full-acceptance.md` no guest.

Nenhuma dessas pendencias e resolvida apenas juntando branches existentes.


## Descoberta de aplicacoes no lunaSH

A continuacao tambem implementa execucao direta de aplicacoes instaladas:
comandos internos tem precedencia; demais nomes sao procurados em PATH
(`/bin:/usr/bin` por padrao), com suporte a caminhos explicitos. O shell
espera o processo e preserva o exit status. PATH e exportado sem duplicacao,
e Tab inclui os executaveis ELF encontrados nos diretorios configurados.
Isso permite registrar Git apenas instalando seu futuro executavel como
`/bin/git`, sem adicionar um alias especifico ao codigo do shell.

A suite QEMU foi executada com sucesso com o fixture ampliado: instalacao
apos boot, autocomplete por teclado, PATH customizado e sua precedencia,
entradas vazias, caminho absoluto/relativo, codigos 40/126/127 e rejeicao de
redirecionamento nativo sem truncar o destino. A suite completa tambem
passou ate a verificacao de metadata apos reboot.

Pipes/redirecionamentos de processos nativos ainda dependem da integracao
de descritores com o shell; a descoberta generica de aplicacoes nao altera
essa pendencia. Detalhes de uso em `docs/developer-api.md`.
