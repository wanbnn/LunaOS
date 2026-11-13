# Matriz de aceitacao de Git upstream no lunaSH

Atenção: esta matriz **define testes futuros; não atesta nenhuma implementação**. O contrato e executar Git upstream no sistema convidado em QEMU, registrar o comando e comparar com o resultado do mesmo Git/versionamento em Linux.

| Etapa | Comandos essenciais (sem mudar sintaxe) | Evidencia necessaria |
| --- | --- | --- |
| Processo | `git --version`, `git help`, `git -C DIR status` | Binario Git upstream roda no guest, suporte a argv/env/stdio/exit |
| Offline basico | `git init`, `git config --global`, `git add -A`, `git commit`, `git status -sb`, `git log --oneline` | Objetos, indice, refs, identidade e persistencia apos reboot |
| Historico | `git diff`, `git show`, `git tag`, `git reflog`, `git fsck`, `git gc` | Comparacao byte a byte de objetos com host, sem perda de historico |
| Branches | `git branch`, `git switch`, `git checkout`, `git merge`, `git rebase -i` | Fluxos com e sem conflito; manutencao de arquivos nao rastreados |
| Edicao de historico | `git reset`, `git revert`, `git cherry-pick`, `git stash` | Recuperacao reversivel e codigo de saida correto |
| Git internals | `git cat-file`, `git hash-object`, `git rev-parse`, `git for-each-ref` | Compatibilidade completa de formato |
| Transporte HTTPS | `git clone https://...`, `git fetch`, `git pull`, `git push` | HTTP smart protocol, TLS verificado, autent. por token e packfiles |
| Transporte SSH | `git clone git@host:org/repo.git`, `git push` | Host keys/Ed25519 validos; rejeicao de identidade desconhecida |
| Avancado | `git submodule`, `git worktree`, `git bundle`, `git am`, `git bisect` | Suporte a auxiliares e shell scripts (quando aplicavel) |
| Interoperabilidade | repo criado em LunaOS aberto em Linux e vice-versa | Mesmo SHA de objetos, refs, trees, packfiles e index |
| Seguranca/estabilidade | falha de rede/disco, TLS invalido, lock concorrente | Nunca corromper repositorio; nenhum bypass silencioso |
| Extensoes | `git lfs`, assinatura de commits, helpers | So aceitar depois de portar os executaveis externos necessarios |

Nenhum resultado de `make check` no host, isoladamente, pode substituir estes testes do binario Git no guest.
