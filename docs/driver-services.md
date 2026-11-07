# Serviços nativos para drivers — API v1

Implementação em C freestanding, executada no kernel LunaOS; não depende de Linux,
GTK ou processos do host. O contrato público está em `kernel/driver.h`. Esta é uma
base nativa para **adaptação de código-fonte** dos drivers; não implementa a ABI,
os headers ou a totalidade das APIs de drivers Linux. Não é possível compilar
`rtlwifi` sem adaptar as dependências restantes.

## O que funciona

| Serviço | Implementação e contrato |
| --- | --- |
| Memória de driver | Pool próprio de 2 MiB, separado do allocator JS, validado contra E820, zero na alocação, páginas físicas contíguas, liberação e rejeição de handles DMA obsoletos |
| DMA coerente | Endereço CPU e endereço de barramento separados; máscara, alinhamento e fronteira solicitados; implementação x86 com identidade física, sem IOMMU |
| PCI | Busca multifunção, revisão/subsistema, posse exclusiva, leitura/escrita de configuração de 8/16/32 bits, sondagem/restauração de BARs, enable separado de bus master |
| MMIO | Arena virtual de 16 MiB em `0x3c000000`, PTEs supervisor com PCD/PWT, partilhada pelos CR3s, acesso de 8/16/32 bits com limites/alinhamento e barreiras |
| Interrupções | Entradas IDT 33–47, PCI INTx compartilhado, PIC/ELCR, EOI de slave/master, detecção de IRQs espúrias 7/15, limite para tempestade de IRQs não atendidas |
| Sincronização | Lock IRQ-save de tentativa única para CPU única, restauração de IF, completion contável e espera com timeout; tentativa de bloquear em IRQ/worker retorna erro |
| Trabalho diferido | FIFO de até 64 trabalhos pendentes, supressão de duplicatas, cancelamento, reenvio e orçamento por iteração |
| Tempo | PIT monotônico em milissegundos, até 32 timers de disparo único com callbacks diferidos, cancelamento; `udelay` de até 1 ms calibrado por TSC/PIT |
| Pacotes | Buffers DMA, headroom/tailroom, push/pull/put, referências, FIFO limitada a 64 entradas e transferência explícita de propriedade |
| Firmware | Arquivo binário separado no disco, até 16 entradas, 512 KiB por arquivo e 2 MiB totais, nomes relativos, checagem de integridade e liberação |

O heap e os metadados são limitados; falhas retornam `DRV_ENOMEM`/`DRV_ENOSPC`.
Não há fallbacks que anunciem sucesso para operações não implementadas.
`drv_pci_msi_enable` retorna `DRV_ENOTSUP`. IRQs sem roteamento legado utilizável
também retornam `DRV_ENOTSUP`.

## Contextos e lifetime

- Inicializar objetos/handles com `{0}`. Não copiar handles de propriedade para
  tentar liberar o mesmo recurso duas vezes. Inicializadores de work/timer são
  usados antes do registro, nunca enquanto o objeto está pendente/em execução.
- `driver_services_init()` roda uma vez no boot, depois da paginação.
  O loop de kernel chama `driver_services_poll(16)`.
- O kernel continua uniprocessado. `drv_lock` desabilita IRQs e falha com
  `DRV_EBUSY` em aquisição recursiva; **não é um spinlock SMP**. Não ceder CPU,
  dormir ou executar operações demoradas segurando esse lock.
- Handlers de IRQ rodam com interrupções desabilitadas. Devem conferir se a
  interrupção pertence ao dispositivo, reconhecer/máscarar a causa no hardware,
  enfileirar trabalho e retornar `true` se a atenderam. Todos os handlers da linha
  são consultados. Não carregar firmware, mapear BARs, dormir ou remover handlers
  nesse contexto. Uma linha com 32 interrupções consecutivas não atendidas é
  mascarada para evitar travar o sistema.
- PIC legado tem limitações: IRQs 0–2, 8, 13 e 14–15 ficam reservadas ao sistema;
  IRQs de PCI em 3–7 e 9–12 são aceitas se o BIOS fornecer line/pin válidos.
  Não há roteamento ACPI/APIC/MSI nem detecção automática de toda placa física.
- Callbacks de work/timer rodam em ring 0 com IRQs habilitadas, cooperativamente,
  não em threads independentes. Devem terminar rapidamente e não bloquear.
  O orçamento limita a quantidade de callbacks, não preempta um callback lento.
- Cancelamento bem sucedido permite destruir work/timer. `DRV_EBUSY` significa
  callback em execução: o objeto e seus argumentos devem continuar vivos.
  Timer pode se rearmar; vencimento usa comparação de 32 bits com wrap e atraso
  máximo `INT32_MAX` ms. Não há timer periódico implícito.
- `drv_wait` processa trabalho e cede ao scheduler; consome um sinal de completion
  ou retorna timeout. Em IRQ, worker ou com IF=0 retorna `DRV_EDEADLK`.
- A alocação de até uma página é permitida em IRQ, com custo limitado; blocos
  maiores exigem contexto de processo. Não usar `drv_free` sobre memória que não
  veio do pool. Liberação de DMA exige que o dispositivo tenha parado de usá-la.
- Pacotes podem compartilhar referências fora das filas, mas mutações exigem
  uma referência única. Enfileirar exige propriedade exclusiva e transfere essa
  referência à fila. Não liberar/modificar enquanto enfileirado. Dequeue devolve
  a propriedade ao chamador; purge libera todos os pacotes ainda enfileirados.

## Ordem para adaptar um driver PCIe

1. `drv_pci_find`: validar vendor/device, revisão, subsistema e requisitos do driver.
2. `drv_pci_claim`: recusa dispositivo já tomado ou já operando como bus master.
3. Sondar BARs; `drv_pci_map` cria o mapeamento sem cache. BAR de I/O é reportado
   por `drv_pci_bar`; usar o endereço com os acessos `inb/inw/inl/outb/outw/outl`
   do kernel. Não sondar BAR enquanto bus master estiver ativo.
4. Habilitar decode com `drv_pci_enable`. Validar o chip pelo hardware antes de
   usar sequências de energia, firmware, efuse e calibração do driver portado.
5. Solicitar firmware e reservar todos os anéis/buffers DMA, respeitando a máscara
   do dispositivo. Adicionar os handlers INTx ainda com o rádio mascarado.
6. Configurar anéis e barreiras. Só então habilitar bus master e interrupções do
   dispositivo. Barreiras não substituem o readback exigido por escritas postadas.
7. Na remoção: impedir novas operações, mascarar/parar o dispositivo e DMA,
   desabilitar bus master, cancelar timers/work, remover handlers, liberar buffers,
   desfazer MMIO e liberar a posse PCI. Tratar `EBUSY` sem destruir objetos ativos.

BARs MMIO de 64 bits localizados abaixo de 4 GiB são aceitos se o tamanho couber;
endereços acima de 4 GiB, BARs de bridge e recursos sobre RAM utilizável não são.
Mappings nunca são user-accessible. A identidade de RAM/DMA e o modelo de cache
são específicos deste backend x86; outro barramento/IOMMU exige outro backend.
A API não torna acesso direto a registradores ou estruturas de driver ring 3 seguro.

## Firmware binário

Colocar os binários licenciados em `assets/firmware/`, por exemplo
`assets/firmware/rtlwifi/rtl8723befw_36.bin`. `make` usa `tools/firmwarepack.py`
para empacotar `.bin`, `.ucode` e `.fw`. A imagem padrão ainda não contém firmware
Realtek/Intel/MediaTek. As licenças e revisão apropriadas precisam acompanhar sua
inclusão. Não basta renomear o firmware de outro chipset.

A região começa no setor 16384 (8 MiB), fora das duas cópias LunaFS. Header de
512 bytes: magic de 8 bytes `LUNAFW1\0`, version/count/total/dir_hash u32 LE. Cada
entrada de 128 bytes contém nome NUL de até 95 bytes, offset/size/FNV32 u32 LE e
20 bytes reservados. Offset é relativo ao início do arquivo, alinhado a 512 bytes.
O loader valida limites, duplicatas, sobreposição, diretório e payload. Firmware
retornado é uma cópia própria; `drv_firmware_release` libera a memória.

FNV32 detecta corrupção acidental, **não autentica firmware**. Origem, versão e
integridade criptográfica devem ser verificadas antes de adicionar o binário à
build. Nunca carregar de um SSID/URL de rede sem validação.

## Validação dentro do guest

```sh
make check
LUNA_TEST_DIR=build/services-test PYTHONPATH=.tools/python python3 tests/driver_services.py
```

O teste inicia QEMU com `-device edu`, injeta um arquivo binário de teste de 70 KB
somente em uma cópia da imagem, executa `drvtest` duas vezes, depois corrompe um
byte e verifica a rejeição. O guest programa BAR, DMA e registradores EDU; dados
passam RAM → dispositivo → RAM, com IRQs reais via PIC. Não é mock de `pci_read`,
`memcpy` simulando DMA ou teste de driver Linux do host.

O relatório é `dist/driver-services-report.json`, associado ao SHA da imagem.
Os arquivos/firmwares de teste não entram na imagem distribuída. No terminal:

```text
drvinfo
drvtest
fwinfo rtlwifi/rtl8723befw_36.bin
```

Sem EDU ou firmware de teste, `drvtest` informa SKIP nesses cenários; isso não
comprova DMA de hardware nem um rádio funcional. O teste automatizado exige
os dois fixtures e falha se algum for ignorado.

## Próxima integração

Para portar `rtlwifi` ainda faltam os adaptadores Linux específicos, equivalentes
à camada MAC (`ieee80211_hw`, `sk_buff` completo, estados, scan, associação,
regulação, agregação e gestão de chaves), WPA2/WPA3 e integração IP por interface.
Esta entrega não anuncia suporte funcional a nenhuma das 15 placas. USB/HCI e
Bluetooth permanecem separados. O EDU valida os serviços inferiores; RF, firmware
Realtek e interoperabilidade com APs precisam de testes físicos independentes.
