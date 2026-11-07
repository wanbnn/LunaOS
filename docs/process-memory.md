# RAM por processo

A aba **Processes** do Task Manager agora apresenta **RAM**, em vez de somente
**Stack**. O comando `ps` apresenta RAM, heap e pilha separadamente, em KiB.
A aba **Memory** usa os contadores reais do gerenciador físico.

## O que o número representa

Para cada PID de aplicação:

```
RAM = páginas físicas das arenas de heap do PID
    + pilha do kernel reservada para a tarefa
    + pilha do cliente
    + diretório e tabela de páginas privados
```

As arenas de heap são exclusivas de um PID. Alocações não utilizam os blocos
livres de arenas pertencentes a outro processo. Uma arena cresce pela aquisição
de páginas do gerenciador físico e devolve essas páginas quando esvaziada.
`RAM` inclui o espaço reservado da arena e seus metadados; `HEAP KiB` no `ps`
mostra esse compromisso físico, não apenas os bytes de objetos vivos. O alocador
também mantém os bytes vivos e o número de alocações para diagnóstico.

Não há quota fixa de RAM por PID. O conjunto de páginas físicas disponíveis é
comum ao sistema, assim como em qualquer alocador físico: um processo que cresce
reduz a disponibilidade para os demais. Falta de RAM, fragmentação e os limites
de endereçamento continuam podendo fazer uma alocação falhar.

O processo do kernel contabiliza código/dados compartilhados, buffers de
composição/serviços, estruturas de tarefas ociosas, DMA, metadados do alocador,
a pilha de bootstrap e, em UEFI, a imagem de disco mantida em RAM. As arenas e
pilhas atribuídas aos PIDs ativos são subtraídas dessa conta para evitar dupla
contagem. Memória reservada pelo firmware não é inventada como RAM de processo.

## Contexto e ciclo de vida

- `heap_allocate` obtém o PID responsável pelo serviço. Antes do scheduler,
  alocações são atribuídas ao futuro PID 1 do kernel.
- O contexto de atribuição é armazenado por tarefa, não em uma variável global
  que possa vazar de um processo para outro quando o scheduler alterna tarefas.
- Desenho e inicialização de um app pelo compositor entram temporariamente no
  contexto do PID daquele app. DOM, CSS, imagens e JavaScript do navegador são
  cobrados do navegador mesmo quando o trabalho é feito pelo compositor.
- `realloc` preserva o PID dono do bloco. `free`, mesmo chamado por outro
  serviço, reduz os contadores do dono original.
- Ao encerrar uma tarefa, os callbacks/globais do backend são limpos primeiro;
  em seguida, todas as arenas daquele PID são liberadas. O navegador libera
  também os caches de documento/estilo e seus ponteiros, para reabrir com um PID
  novo sem referências a memória liberada.
- Pilhas e tabelas de páginas ainda usam buffers estáticos pré-reservados. Ao
  encerrar o cliente, esses buffers permanecem no kernel para reutilização;
  a RAM do heap é efetivamente devolvida ao gerenciador físico.

## Limite do isolamento atual

Essa separação é de propriedade física e contabilidade. Os backends dos apps
continuam sendo serviços executados em modo supervisor; as arenas não são heaps
mapeados para acesso direto pelo cliente ring 3. Portanto, não se deve apresentar
isso como um espaço de endereçamento privado completo ou uma sandbox de código
C por processo. As proteções existentes de pilha/páginas/segmentos dos clientes
permanecem, e código/buffers globais dos serviços pertencem ao kernel.
A recuperação de falta de memória ainda depende do backend: alguns serviços
ainda tratam falhas como erro de kernel, portanto essa contabilidade não deve
ser confundida com isolamento completo de falhas/OOM por processo.

## Validação

- `make test-heap`: arenas exclusivas por PID, contadores, `realloc` preservando
  o dono, liberação em outro contexto, encerramento de um PID preservando os
  outros, ausência de limite de 8 MiB e recuperação integral das páginas.
- `LUNA_TEST_DIR=/tmp/luna-process-ram python3 tests/process_memory.py`: inicia o
  kernel no QEMU, carrega uma página que mantém um buffer de 8 MiB e verifica
  que o aumento aparece no navegador, sem crescer novamente na linha do kernel
  ou na conta do shell. Verifica navegação, encerramento, reabertura com novo PID
  e gera `dist/process-ram.png` e `dist/process-memory-report.json`.
- `LUNA_TEST_DIR=/tmp/luna-process-tasks python3 tests/multitask.py`: proteções
  dos rings, PIDs, janelas, encerramento e reabertura de tarefas.
