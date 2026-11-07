# Memória física sob demanda

O tamanho de RAM configurado no launcher ou no teste QEMU não é um limite do
kernel. Ao iniciar em hardware, a capacidade vem do mapa E820 (BIOS) ou de sua
tradução a partir do `GetMemoryMap` do UEFI. O loader transporta até 128 regiões.
Entradas desabilitadas são ignoradas; regiões reservadas prevalecem sobre entradas
sobrepostas de RAM utilizável. Overflow e mapas inválidos são rejeitados.

## Alocação

- `kernel/pmm.c`: alocador de páginas físicas contíguas de 4 KiB. Seus metadados
  são dimensionados pela extensão física reportada, reservados na própria RAM
  e mantêm tamanho/cookie para detectar handles obsoletos e liberações inválidas.
- `kernel/memory.c`: integra o mapa ao kernel, com exclusão do kernel/BSS,
  firmware, framebuffer, imagem de boot UEFI, pilha de bootstrap e regiões cujos
  mapeamentos virtuais substituem a identidade física. Todas essas reservas são
  contabilizadas, não apresentadas como memória disponível. O intervalo entre
  os trechos baixo e alto do kernel é liberado quando o firmware o classifica
  como RAM; regiões ACPI/NVS nesse intervalo continuam reservadas.
- `drv_dma_alloc`: usa o mesmo alocador físico, com alinhamento, máscara e
  fronteira do dispositivo, e inicializa o buffer com zeros. O pool fixo de
  2 MiB deixou de existir. A liberação exige que o hardware tenha parado de
  acessar a memória; a API não pode inferir isso de um ponteiro.
- `kernel/heap.c`: arenas crescem sob demanda para atender `malloc`; blocos
  livres adjacentes na mesma arena são coalescidos e arenas vazias devolvem
  as páginas ao gerenciador físico. O heap estático de 8 MiB deixou de existir.
  A granularidade de crescimento de 64 KiB não limita o tamanho total do heap.
- O runtime CLI e o runtime JavaScript do navegador usam esse heap. A quota
  fixa de 4 MiB por runtime foi removida; as restrições de pilha e tempo de
  execução permanecem próprias dos serviços atuais.

O kernel atual é uniprocessador. Atualizações dos metadados são protegidas
pela preservação do estado de IRQ; o buffer DMA é limpo fora da seção crítica.
Drivers e heap disputam páginas do mesmo conjunto livre. Alocação não garante
sucesso se houver fragmentação ou se a máscara do dispositivo não alcançar
as páginas livres; não há fallback que anuncie um endereço fictício.

## Limite de arquitetura, sem teto de RAM configurado

O kernel continua sendo i386 com paginação sem PAE. Ele pode alocar RAM
utilizável nos endereços físicos abaixo de 4 GiB. O total utilizável reportado
pelo firmware inclui RAM acima desse endereço, mas essa parcela é apresentada
como indisponível para alocação, até implementar PAE ou migrar para x86-64.

Isso significa que uma máquina com 4 GiB instalados pode oferecer menos de
4 GiB abaixo desse endereço: firmware/MMIO ocupam espaços e parte da RAM pode
ser remapeada para acima de 4 GiB. Não seria correto prometer uso integral dessa
parcela com a paginação atual. O mapa do firmware, não o nome do processador,
determina quais regiões são RAM.

As janelas virtuais atuais em `0x3c000000..0x3d000000` e
`0x40000000..0x40400000` substituem mapeamentos de identidade. RAM física nesses
endereços fica reservada enquanto a API de CPU/DMA usa ponteiros físicos diretos.
O bitmap considera as demais regiões abaixo de 4 GiB, inclusive acima de 64 MiB,
256 MiB ou 1 GiB. A próxima evolução para recuperar essas reservas e alocar
páginas não contíguas em blocos virtuais contíguos é um gerenciador de memória
virtual com mapeamento separado para heap e endereços físicos.

BSS, superfícies gráficas e algumas tabelas de tarefas continuam estáticas.
As arenas do heap agora têm proprietário por PID e são contabilizadas no
Task Manager, sem quotas fixas; o fornecimento de páginas físicas continua comum.
Isso não implementa heaps de usuário mapeados em espaços de endereçamento
privados. Detalhes: [RAM por processo](process-memory.md).

## Diagnóstico e testes

- `free`: informa RAM utilizável do firmware, RAM endereçável, reservas,
  páginas alocadas/livres e custo de metadados.
- `memtest 96`: aloca 96 MiB no total, divididos entre heap e DMA, grava e
  verifica padrões por página, libera os buffers e verifica o retorno da memória.
  Em máquinas pequenas, use `memtest 4`; falhas de alocação liberam os recursos.
- `make test-pmm test-heap`: mapas simulados de 64/256 MiB e 4 GiB, sobreposições,
  reservas, falhas, máscaras/fronteiras DMA, cookies, esgotamento, heap acima
  de 8 MiB, alinhamento e coalescência. Esses são testes em host.
- `LUNA_TEST_DIR=/tmp/luna-memory-test python3 tests/memory.py`: boot real do
  kernel no QEMU com 64, 256 e 4096 MiB, escrita/leitura/devolução de 4 ou
  96 MiB e JavaScript com um `Uint8Array` de 12 MiB. Requer `make all`.
- `LUNA_UEFI=1 LUNA_TEST_DIR=/tmp/luna-memory-uefi python3 tests/memory_uefi.py`:
  verifica boot UEFI, preservação de ACPI/NVS e da imagem de boot, 96 MiB de
  alocação, leitura do firmware Intel e JavaScript acima da antiga quota.
- `LUNA_TEST_DIR=/tmp/luna-driver-test python3 tests/driver_services.py`: DMA
  bidirecional real com o EDU do QEMU, máscaras, alinhamento, rejeição de
  handles obsoletos, IRQ e corrupção de firmware.

No QEMU, a RAM disponível é aquela atribuída à VM: `LUNA_RAM=4096 make run`
configura 4 GiB. Isso não expõe ao guest o restante da RAM do host.

Referências de formato:
[ACPI E820](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/15_System_Address_Map_Interfaces/int-15h-e820h---query-system-address-map.html)
e [tradução UEFI](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/15_System_Address_Map_Interfaces/uefi-getmemorymap-boot-services-function.html).
