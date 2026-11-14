# Estado do porte Git upstream — 13/11/2026

**Git 2.56.1 executa nativamente no LunaOS e esta incluido em `/bin/git`.**
Dash upstream 0.5.12 fornece `/bin/sh`. O porte completo ainda nao foi
concluido: a camada HTTP está em desenvolvimento, mas HTTPS/SSH e toda a matriz avancada permanecem sem validação integral.

Uso, builds e limites: [git-native.md](git-native.md).

## Evidencia atual no guest

A suite `tests/git_native_guest.py` usa o Git da propria imagem, sem baixar
um Git do host nem delegar operacoes de repositorio a servicos externos.
Passaram os fluxos init/config/add/commit/log/status, tags, branches e merge
fast-forward, hash de blob comparado ao formato Git, fsck e leitura depois
de reboot. Tambem passaram clone local, clone bare, push, fetch e pull entre
repositorios locais usando os auxiliares Git e pipes reais.

O fixture completo de transporte local executa com 128 MiB. Os fluxos
pequenos offline e clone local foram exercitados com 64 MiB. Push com tres
ou mais processos pode esgotar os 64 MiB; o perfil completo recomenda
128 MiB. A configuracao padrao desabilita manutencao automatica para evitar
picos em background. Nao ha promessa de suportar grandes repositorios com
os mesmos limites.

Um programa C independente testa sinais bloqueados/entregues, terminacao
por sinal, fork com heap privado, pipe/dup2, execve com Git e Dash e
redirecionamento para /dev/null. Os arquivos e metadados persistem depois
de sync/reboot. Nao ha resultado inventado para um comando do Git.

## Implementacao

- Git upstream fixado em `03b41523c576f26ad34085378717a0c1b287470f`.
- Dash fixado em `4bbf8721a3ac6401ced6a0454956801f6ba37256`.
- Sysroot musl isolado e ABI adaptada em `sdk/musl-luna`.
- ELF32 em ring 3; codigo imutavel compartilhado entre processos, segmentos
  gravaveis privados; heap brk de ate 4 MiB e stack de 128 KiB.
- Fork, execve, wait de filho especifico/qualquer filho, sinais basicos,
  sessoes, pipes, poll e descritores com close-on-exec e stdio redirecionado.
- Lockfiles O_EXCL, leitura/escrita em offsets, metadados persistentes,
  dispositivos ABI /dev/null e /dev/urandom e timestamps do clock do kernel.
- Executaveis comprimidos em mounts imutaveis, excluidos do snapshot
  gravavel. Nao consomem os 2 MiB do volume legado para guardar a CLI.
- Licencas e caminhos do codigo-fonte incluidos em /etc/licenses.

A ISO continua com aproximadamente 66,125 MiB; o kernel, incluindo os
payloads comprimidos, permanece abaixo da reserva de 8 MiB do bootloader.

## Analise das branches anteriores

A main local foi atualizada de `82448a7` para `0fccc60` e recebeu a continuacao
de infraestrutura/lunaSH em `509f94f`. `origin/feat/full-upstream-git-port`
ja era ancestral da main pelo PR #8. A branch
`feat/lunash-native-git-foundation` permanece uma implementacao parcial
antiga, nao utilizada por este executavel. O outro worktree
`feat/git-upstream-luna-runtime` foi preservado.

O executavel, a libc e o shell desta etapa estao na branch
`feat/git-upstream-native-executable`. Nao confundir os testes locais com CI
remoto ou com conclusao do contrato completo de Git.

## Pendencias para o contrato completo

HTTPS smart protocol/libcurl, SSH e host keys; escala de memoria e tamanho
de repositorios; identidade de arquivos abertos apos rename/unlink;
compartilhamento de offsets de descritores regulares; symlinks/hardlinks,
permissoes e locking completos; job control e cobertura adicional de sinais;
extensoes e todos os fluxos de falha/recuperacao do contrato original.
