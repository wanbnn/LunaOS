# Native Storage / block-device API

## Design

O LunaOS continua carregando a imagem UEFI para a memória RAM porque, depois
de `ExitBootServices`, não pode chamar o Block I/O do firmware. Uma imagem
de 66 MiB em RAM **não significa que o pendrive inteiro (ex.: 64 GB)** foi
carregado, nem que existe driver USB no kernel.

O novo subsistema `kernel/storage.{h,c}` fornece a interface unificada
`StorageTransfer(context, lba, sectors, data, write)` e `StorageFlush`,
registro limitado de discos, tipo de controlador, modelo, tamanho em setores
e proteção contra acesso fora dos limites. Os slots 0–3 continuam IDE, 4 é
a cópia UEFI RAM ou mídia óptica, e 5–12 são dispositivos nativos descobertos.

O acesso é síncrono e serializado, com comandos DMA em buffers limitados.
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
| USB xHCI/EHCI/UHCI/OHCI | **Somente inventário** | **Não** | Requer controlador HC, enumeração, hubs, bulk-only/UAS, SCSI |
| SDHCI/eMMC | **Somente inventário PCI** | **Não** | Requer SD/MMC host, inicialização e transferências; eMMC não é NVMe |
| RAID | **Somente inventário PCI** | **Não** | Driver de fabricante/firmware ou software RAID com metadados compatíveis |

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
```

`tests/storage.c` utiliza um backend mock com limites e múltiplos dispositivos
para verificar o contrato sem hardware. `tests/storage_guest.py` inicia o
kernel em QEMU com unidades SATA e NVMe separadas e confirma, **somente
por leitura**, a comunicação do guest com os dois controladores.
Teste em QEMU não comprova compatibilidade em um SSD ou RAID físico.

### Próximas etapas

1. Testes automatizados de **instalação destrutiva em discos QEMU
   descartáveis**, incluindo reinício no destino e verificação de persistência.
2. Controladores USB xHCI e BOT/SCSI e identificação da mídia de boot por
   identificador de dispositivo/partição, não apenas assinatura MBR.
3. SDHCI/eMMC real e RAID específico (Intel RST/VMD, MegaRAID etc.).
4. Partições GPT e filesystem de dados dimensionável, com blocos grandes.
5. Suporte a discos >2 TiB/LBA64, 4Kn, IOMMU, MSI e recuperação de erros.
