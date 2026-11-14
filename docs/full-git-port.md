# Porte do Git original ao LunaOS — contrato de compatibilidade

**Status: Git upstream 2.56.1 e Dash executam nativamente no guest e integram a imagem. Fluxos locais foram validados; HTTPS/SSH e a aceitacao completa continuam pendentes.**

Uso e build atuais: [git-native.md](git-native.md). As secoes de milestones abaixo preservam o historico anterior.

Estado consolidado, branches e verificacoes: [git-port-status.md](git-port-status.md). Este documento define o requisito solicitado: executar o `git` real a partir do `lunaSH` com o mesmo fluxo de trabalho que em sistemas POSIX, incluindo trabalho offline, remotos HTTPS/SSH e interoperabilidade com repositórios criados em outros sistemas. Nenhuma substituicao parcial, emulacao de respostas ou dependencia obrigatoria de um host Linux e aceitavel.

## Decisao arquitetural

**Portar o Git upstream (git/git), nao reimplementar um subconjunto.** libgit2 e uma biblioteca de funcionalidades nucleares e declara explicitamente que nao substitui a ferramenta Git e seus comandos voltados ao usuario. O Git upstream exige zlib, shell POSIX para alguns comandos, libcurl para remotos HTTP(S), SSH para transporte seguro e dependencias opcionais para extensoes (Perl/Tcl etc.). Reproduzir uma CLI completa ao redor de libgit2 expandiria indefinidamente o trabalho de compatibilidade.

Fontes:
- Git upstream INSTALL: https://github.com/git/git/blob/master/INSTALL
- Git upstream CLI reference: https://git-scm.com/docs/git
- libgit2 limitations: https://github.com/libgit2/libgit2

A execucao precisa acontecer no **LunaOS guest**, sem delegar todas as operacoes a um servico remoto ou ao sistema hospedeiro. A construcao cruzada no host e permitida. A versao upstream e os hashes dos artefatos devem ser fixados em um manifesto de origem antes de vendorizacao.

## Diagnostico inicial (historico, anterior a integracao do PR #8)

| Subsistema | Estado verificavel na main | Contrato necessario para Git |
| --- | --- | --- |
| Execucao de programas | Apenas clientes/servicos embutidos e runtimes gerenciados, sem loader geral de binarios POSIX/ELF | ABI executavel de usuario e loader de executaveis Git e auxiliares, sem rodar comando no ring 0 |
| Processos/terminal | `lunaSH` interpreta built-ins, sem processos Unix convencionais | `execve`, `spawn`, `wait`, pipes, redirecionamento, env, sinais e codigos de saida reais |
| libc | musl incorporada seletivamente para runtime de linguagens | libc e camada POSIX suficientes para upstream Git, incluindo I/O e metadata |
| VFS/LunaFS2 | Suporta diretorios e arquivos binarios, mas nao ABI POSIX integral | `open/read/write/close`, `stat/lstat`, `rename` atomico, `mkdir`, `readdir`, `fsync`, `fcntl/lock`, mtime/modo e links simbolicos |
| Build | Ferramentas i386/C e biblioteca statica parcial do host | Toolchain de cross-build e sysroot especifico do LunaOS, com testes de link e execucao guest |
| Rede | TCP/IPv4 e HTTPS parcial via BearSSL | API BSD sockets; libcurl ou backend equivalente para smart HTTP Git v1/v2, streaming, redirects, auth e TLS seguro |
| SSH | Nao ha cliente SSH de uso geral no guest | Transporte Git via SSH, host key checking, ed25519 e chaves autorizadas |
| Compressao | zlib nao esta declarada como dependencia Git do guest | zlib funcional para objetos Git, packfiles e delta streams |
| Identidade/seguranca | Autenticacao de usuario do sistema em desenvolvimento | Usuario, home, configuracao global, permissao de chave SSH, credenciais e armazenamento protegido |
| Interface | linha de comando do lunaSH de 256 caracteres e buffers de saida limitados | argv/env reais, streams incrementais, TTY, interrupcao, saida de grande volume |
| Imagem | meta de 120MB e 64MiB de memoria minima | Medir aumento real da ISO e RAM; nenhuma promessa de caber sem perfis de build |

Arquivos atuais: `apps/shell.c`, `kernel/luna.h`, `kernel/fs.c`, `kernel/task.c`, `drivers/net.c`, `third_party/musl`, `docs/architecture.md`, `docs/developer-api.md`.

## Prioridade de implementacao tecnica (sem anunciar como Git pronto)

1. **ABI/processos:** programa externo real e syscall/posix shim com testes de isolamento, args, env e pipes. O shell despacha `git` como processo, nao como funcao isolada dentro do kernel.
2. **VFS e libc:** metadados, links, rename atomico, locking e fsync; teste de queda de energia; git init/objects nao podem corromper LunaFS.
3. **Build upstream fixada:** vendor de release oficial verificada, patches restritos para `compat/` e plataforma LunaOS, builds `-m32` estaticas para guest, documentacao de licencas e reproducibilidade.
4. **CLI offline completa:** `init`, `config`, `add`, `status`, `diff`, `commit`, `log`, `tag`, `branch`, `switch`, `checkout`, `merge`, `rebase`, `cherry-pick`, `reset`, `revert`, `stash`, `reflog`, `worktree`, `gc`, `fsck`, `bundle`, `submodule` e conflitos. Testar tambem hooks e scripts dependentes de shell.
5. **Remotos sem mudar o fluxo:** `clone`, `fetch`, `pull`, `push`, `remote`, `ls-remote` com smart HTTP/HTTPS, HTTPS auth, SSH e host key validation; packfiles grandes e reinicio apos erro.
6. **Extensoes opcionais e ferramentas externas:** `git lfs`, signing GPG/SSH, credential helpers e comandos externos dependem de seus proprios programas/servicos. Devem ter suporte como mecanismos de extensao, ou limitacoes explicitamente documentadas. Nunca anunciar extensao como parte do core sem testa-la.

## Contrato de aceitacao para merge

Exigir CI verde e teste guest (QEMU) com **Git oficial** executando os mesmos fixtures de regressao em Linux no host e em LunaOS. Os dois ambientes devem produzir resultados compativeis para:

- `git --version` devolve a versao do Git upstream fixada, nao o numero de uma reimplementacao.
- Iniciar repositorio, criar arquivos binarios e grandes, configurar identidades, fazer commits, tags e branches.
- Criar e resolver conflitos em merge/rebase, cherry-pick e revert; stash, reset, reflog e gc; persistir apos reboot.
- Clonar repo HTTP(S) **publico e privado**, autenticar sem exibir credenciais; verificar certificados por padrao.
- Clone/fetch/push em SSH com host key validada e chaves Ed25519; recusar host inesperado.
- Push de branches e tags, pull fast-forward e nao fast-forward, fetch com packfiles/delta, interromper/repetir sem corromper objetos.
- Clone de repositorio externo criado por Git Linux; executar Git Linux no repositorio gerado pelo LunaOS; comparar objetos, refs, indices e conteudos.
- Submodulos, Git hooks, config global/local, .gitignore e attributes, nomes UTF-8, links simbolicos, perms e grandes repos.
- Executar sem servidor intermediario LunaOS ou computador hospedeiro para calculo ou historico local; apenas o servidor remoto Git convencional e permitido para operacoes remotas.

Testes devem cobrir falhas de rede, disco cheio, falta de RAM, TLS invalido, credenciais incorretas, perdas de energia, uso concorrente e rollback. **Sem demonstracao end-to-end no guest, nao ha merge, release ou anuncio de Git completo.**

## Situacao da tentativa parcial anterior

A branch experimental `feat/lunash-native-git-foundation` contem uma tentativa inicial de built-in local que oferece apenas comandos limitados (sem add/commit/push/pull). **Nao integra a main, nao atende ao requisito e nao deve ser mesclada**; o porte upstream substitui essa estrategia.

## Notas de projeto

Nao habilitar login automatico, armazenar PAT em plaintext, desabilitar TLS, ignorar verificacao de host SSH ou enviar codigo dos usuarios para terceiro para simplificar a implementacao. A paridade com Git deve ser demonstrada, nao presumida.


## Progresso executado na fase 1 (13/11/2026)

- Adicionado o codigo upstream oficial do Git via submodulo Git em `third_party/git`, fixado em `03b41523c576f26ad34085378717a0c1b287470f` (tag assinada `v2.56.1`, com correcoes de seguranca).
- Criado `tests/git_upstream_host.py` para comparar o fluxo real de Git no ambiente host: inicializacao, arquivos binarios, add/commit, branch, merge, rebase, stash, clone/fetch/pull/push em remoto local, fsck e gc.
- Criado `git-upstream-host-build` / `git-upstream-host-test` no Makefile e CI `.github/workflows/git-upstream.yml` para compilar o Git original em Linux. Esses alvos nao entram na ISO.
- Documentados o ambiente e as etapas em `tools/git-port/README.md`.

**Ainda não há processo user-mode do Git, ABI POSIX/libc completa, VFS completo ou SSH/HTTPS para Git. O loader ELF32 e parte da infraestrutura de processos ja foram integrados.** Uma compilacao no host, mesmo aprovada, nao e demonstracao de Git funcional no guest. A branch parcial anterior `feat/lunash-native-git-foundation` continua isolada e nao deve ser mesclada.

## Milestone 4 — estado da infraestrutura (13/11/2026)

Implementado na branch de integração: espaço de código/dados ELF32 esparso
de até 8 MiB; carregamento dos segmentos do arquivo diretamente na memória
física sem staging integral; stack separada; syscalls `pipe` com buffers
limitados, `fsync` vinculado a snapshots LunaFS2, criação real de processos
filhos por `luna_spawn` e espera por `luna_waitpid`. O SDK C foi
ampliado para testar estes recursos.

O código upstream zlib v1.4 foi fixado em `third_party/zlib`, SHA
`99623ef7097352da7993ba614161fd6a311e046e`. O build agora dispõe de
uma biblioteca `build/libz-luna.a` para i386 e teste de
compressão/descompressão executado no guest. A versão exata é fixada
por commit; a tag não foi atestada como criptograficamente assinada.

**Ainda bloqueados para Git oficial:** loader estático para executável
de grande porte com endereçamento geral/relocações; heap POSIX escalável;
`execve` verdadeiro, pipes bloqueantes interprocesso, sinais;
metadados POSIX persistentes, symlinks, locks, timestamps e `rename`
com todas as garantias Unix; BSD sockets múltiplos, smart Git HTTP(S)
POST e SSH com host-key verification. Não anunciar Git nativo
nem fazer merge apenas por testes de infraestrutura aprovados.


## Atualizacao da integracao nativa

A main `0fccc60` ja inclui `execve`, heap `brk` de ate 2 MiB com
malloc/realloc/free, pipes bloqueantes herdados, chmod/mtime persistentes,
iteracao de diretorios e um transporte TCP/TLS exclusivo. As listas de
bloqueios dos milestones acima sao historicas; consultar o estado consolidado.

A continuacao adiciona criacao exclusiva (`O_CREAT|O_EXCL`, retorno
`-LUNA_EEXIST`), seek/escrita em offsets arbitrarios e preenchimento de
lacunas com zeros. Arquivos maiores que 64 KiB usam substituicao de snapshot,
com RAM limitada por bloco. Isso nao fornece libc completa nem executa Git.
