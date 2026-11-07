# Native Storage / block-device API

## Design

O LunaOS continua carregando a imagem UEFI para a memória RAM porque, depois
de `ExitBootServices`, não pode chamar o Block I/O do firmware. Uma imagem
de 66 MiB em RAM **não significa que o pendrive inteiro (ex.: 64 GB)** foi
carregado, nem que o controlador USB usado pelo firmware é suportado pelo kernel.

O novo subsistema `kernel/storage.{h,c}` fornece a interface unificada
`StorageTransfer(context, lba, sectors, data, write)` e `StorageFlush`,
registro limitado de discos, tipo de controlador, modelo, tamanho em setores
e proteção contra acesso fora dos limites. Os slots 0–3 continuam IDE, 4 é
a cópia UEFI RAM ou mídia óptica, e 5–12 são dispositivos nativos descobertos.

O acesso é síncrono e serializado, com DMA em buffers limitados ou PIO SDHCI.
Os serviços PCI/DMA agora são inicializados **antes** do `fs_init`, e
`disk_resolve_native_boot` tenta localizar a unidade física pelo ID MBR
fornecido no boot BIOS/UEFI. Sem correspondência única, continua usando a
origem segura (IDE ou cópia UEFI RAM); IDs duplicados não são aceitos como
seleção automática. O firmware UEFI ainda entrega a imagem de 66 MiB em RAM,
mas o LunaFS já pode acessar a mídia nativa após encontrar o dispositivo.
A origem de boot não deve ser apresentada como destino de instalação; o
instalador também compara o identificador MBR para excluir clones.
Armazenamento persistente continua sob LunaFS2, com 2 MiB por snapshot, mesmo
quando o disco é maior. Para aproveitar o espaço do pendrive/SSD, ainda falta
uma partição de dados com gerenciamento de espaço e drivers correspondentes.

## Estado por controlador

| Driver | Status | I/O | Observações |
| --- | --- | --- | --- |
| ATA/IDE | Existente | LBA28 PIO | 4 posições de IDE |
| AHCI/SATA PCI class `010601` | **Experimental** | IDENTIFY, LBA48 DMA EXT, cache flush | BAR5 abaixo 4 GiB, 512B lógico, 64 setores por comando, polling |
| NVMe PCI class `010802` | **Experimental** | Identify ctrl/ns1, fila admin+I/O, leitura, gravação, flush | 4 KiB PRP, 512B LBA, 8 setores por comando, polling; sem namespaces adicionais |
| USB EHCI (`0c0320`) | **Experimental** | Enumeração, BOT/SCSI INQUIRY, READ CAPACITY, MODE SENSE, READ/WRITE(10), SYNCHRONIZE CACHE | High-speed em porta raiz, LUN0, 512 B, 32 setores por comando; sem hubs, UAS, xHCI/OHCI/UHCI |
| SDHCI/eMMC (`080500/080501`) | **Experimental** | CMD0/1/2/3/7/8/9/13/17/24, SD ACMD41, eMMC EXT_CSD/cache flush | PIO 1-bit até 25 MHz; SDHC/SDXC e eMMC com OCR sector mode, 512 B, área de usuário |
| RAID MegaRAID MFI (`1000:0060/0079`) | **Experimental** | Inicializa fila, lista volumes, SCSI READ CAPACITY/MODE SENSE, LD READ/WRITE, DCMD cache flush | SAS1078/2108; volumes existentes, 512 B; não cria arrays nem escreve em membros físicos |

**Nenhum backend de instalação é automaticamente universal.** IOMMU, MSI,
APIC, BAR acima de 4 GiB, NVMe 4K lógico, hotplug, comandos assíncronos,
SATA NCQ, NVMe namespaces adicionais, controladores SAS e Intel VMD
permanecem fora deste marco. Controladores RAID não expõem necessariamente
discos legíveis por AHCI ou NVMe.

### Verificação sem escrita

```sh
storage
storage verify 5
storage verify 6
```

`storage verify N` compara duas leituras reais do setor 0 e imprime um
checksum FNV; ele **não** modifica partições e **não** prova gravação.
A instalação propriamente dita apaga a tabela de partições e deve ser testada
somente com unidades QEMU descartáveis. Para navegar pelo setup, use as
setas esquerda/direita para trocar de página de discos.

### Desenvolvedores de drivers

```c
StorageDevice device = {
    .ctx = controller,
    .transfer = my_driver_transfer,
    .flush = my_driver_flush,
    .sectors = total_sectors,
    .logical_bytes = 512,
    .transport = STORAGE_AHCI,
    .readonly = false,
    .installable = false, // só habilitar depois da validação do backend!
};
strcpy(device.model, "My disk");
int index = storage_register(&device);
```

O driver físico reivindica PCI, configura DMA, MMIO e flush via
[`kernel/driver.h`](../kernel/driver.h); a camada de blocos **não**
concede acesso a hardware privilegiado para scripts JS/Python.
O registro não disponibiliza um carregador de módulos de kernel.

### Validação

```sh
make test-storage
make -j2 all
make test-storage-guest
make test-storage-interfaces
make test-storage-install
```

`tests/storage.c` utiliza um backend mock com limites e múltiplos dispositivos
para verificar o contrato sem hardware. `tests/storage_guest.py` inicia o
kernel em QEMU com unidades SATA e NVMe separadas e confirma, **somente
por leitura**, a comunicação do guest com os dois controladores.
Teste em QEMU não comprova compatibilidade em um SSD ou RAID físico.

### Escopo e validação dos novos backends

`tests/storage_interfaces.py` verifica leituras físicas e SHA-256 da imagem inteira
antes/depois para EHCI/BOT, SDHC, eMMC e MegaRAID SAS1078/2108.
`tests/storage_install.py` executa o instalador com esses cinco dispositivos e
AHCI/NVMe: confirmação do disco 5, cópia de 66 MiB com leitura de retorno,
flush e commit do setor de boot. As imagens SD/eMMC de 4 GiB são esparsas;
QEMU anuncia modo de endereçamento por setor somente acima de 2 GiB.
O cenário eMMC exige o modelo `emmc` (validado com QEMU 10.2). Em versões
antigas o conjunto padrão informa SKIP; pedir `LUNA_STORAGE_KINDS=emmc`
explicitamente exige esse modelo e falha caso esteja ausente.
`LUNA_STORAGE_KINDS="usb sdhci emmc raid raid2 ahci nvme"` permite selecionar
os cenários. O teste destrutivo só cria arquivos em `build/test`.

EHCI realiza handoff BIOS/OS, reset de porta, enumeração de descritores e
seleção de interface SCSI/BOT. Valida CSW, tag, status e resíduo. Recupera
erros de transporte com Bulk-Only Reset e CLEAR_FEATURE(ENDPOINT_HALT), sem
repetir gravações automaticamente. Falha de conclusão DMA desabilita o host;
os buffers permanecem reservados, impedindo reutilização de DMA pendente.
Mídias protegidas contra escrita não são instaláveis. Falha no comando de
sincronização de cache também impede instalar em uma unidade USB.

SDHCI verifica erros R1 (inclusive eMMC SWITCH_ERROR), limites da mídia,
proteção CSD e sinal de proteção contra escrita. Espera
READY_FOR_DATA/TRAN após escrita; eMMC com cache ativo recebe FLUSH_CACHE.
Não muda boot partitions, RPMB, largura do barramento ou modo HS200/HS400.
O MFI publica somente volumes online/degradados com capacidade SCSI de 512 B;
MODE SENSE verifica proteção contra escrita do volume lógico. A
disposição dos membros e a paridade continuam sob responsabilidade do
firmware RAID. Nenhum comando de configuração de array é emitido.

Os testes em QEMU não validam hardware físico, reinicialização pelo disco de
destino nem recuperação de falta de energia. Permanecem pendentes xHCI,
hubs, UAS, Intel RST/VMD, software RAID, GPT, 4Kn, LBA64 e filesystem escalável.

Referências de protocolo e modelos usados para validação:
[EHCI/QEMU](https://github.com/qemu/qemu/blob/master/hw/usb/hcd-ehci.c),
[SDHCI/QEMU](https://github.com/qemu/qemu/blob/master/hw/sd/sdhci.c),
[SD/eMMC/QEMU](https://github.com/qemu/qemu/blob/master/hw/sd/sd.c),
[MegaRAID/QEMU](https://github.com/qemu/qemu/blob/master/hw/scsi/megasas.c),
[MFI](https://github.com/qemu/qemu/blob/master/hw/scsi/mfi.h).
