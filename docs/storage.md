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
O LunaFS2 expande o diretório dinamicamente e pode criar uma partição de dados
no espaço livre após a imagem do sistema, preservando as partições existentes.
A capacidade persistente depende do espaço disponível e usa dois snapshots.
Consulte [LunaFS2](lunafs2.md).

## Estado por controlador

| Driver | Status | I/O | Observações |
| --- | --- | --- | --- |
| ATA/IDE | Existente | LBA28 PIO | 4 posições de IDE |
| AHCI/SATA PCI class `010601` | **Experimental** | IDENTIFY, LBA48 DMA EXT, cache flush | BAR5 abaixo 4 GiB, 512B lógico, 64 setores por comando, polling |
| NVMe PCI class `010802` | **Experimental** | Identify ctrl/ns1, fila admin+I/O, leitura, gravação, flush | 4 KiB PRP, 512B LBA, 8 setores por comando, polling; sem namespaces adicionais |
| USB EHCI (`0c0320`) | **Experimental** | Enumeração, BOT/SCSI INQUIRY, READ CAPACITY, MODE SENSE, READ/WRITE(10), SYNCHRONIZE CACHE | High-speed em porta raiz, LUN0, 512 B, 32 setores por comando; sem hubs, UAS, xHCI/OHCI/UHCI |
| SDHCI/eMMC (PCI ou ACPI `_HID`/`_CRS`) | **Experimental** | CMD0/1/2/3/7/8/9/13/17/24, SD ACMD41, eMMC EXT_CSD/cache flush | PIO 1-bit até 25 MHz; SDHC/SDXC e eMMC com OCR sector mode, 512 B, área de usuário |
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

Os testes em QEMU não certificam hardware físico, boot pelo firmware de cada
controlador nem recuperação de falta de energia. Permanecem pendentes xHCI,
hubs, UAS, Intel RST/VMD, software RAID, GPT, 4Kn, LBA64 e snapshots com comprimentos de 64 bits.

Referências de protocolo e modelos usados para validação:
[EHCI/QEMU](https://github.com/qemu/qemu/blob/master/hw/usb/hcd-ehci.c),
[SDHCI/QEMU](https://github.com/qemu/qemu/blob/master/hw/sd/sdhci.c),
[SD/eMMC/QEMU](https://github.com/qemu/qemu/blob/master/hw/sd/sd.c),
[MegaRAID/QEMU](https://github.com/qemu/qemu/blob/master/hw/scsi/megasas.c),
[MFI](https://github.com/qemu/qemu/blob/master/hw/scsi/mfi.h).

### eMMC PCI em equipamentos reais (Intel / HP Stream)

Os IDs Intel dedicados de Bay Trail, Braswell, Apollo Lake e Gemini Lake
são tratados como eMMC soldado, inclusive quando a classe PCI não descreve
SDHCI. Esses hosts não dependem do sinal de presença de cartão ou do pino WP
usado por leitores SD removíveis. A proteção permanente/temporária CSD e os
erros de comando continuam sendo respeitados.

A inicialização solicita D0 pela capability PCI PM antes de mapear os
registradores. Hosts SDHCI v3 usam o divisor de 10 bits: 200 MHz podem ser
reduzidos a 400 kHz sem exceder o clock de identificação. A alimentação de
1,8 V só é selecionada em hosts eMMC dedicados que a anunciam; não se emite
CMD11 de SD nem se escreve em GPIO/reguladores sem descrição da plataforma.

`make test-storage-sdhci` usa um modelo de registradores com eMMC de 64 GiB,
sem CD/WP, alimentação de 1,8 V, clock de 200 MHz e estado inicial D3. Verifica
leitura/escrita, flush do cache e diagnóstico de timeout. `tests/storage_emmc_iso.py`
valida a ISO com 4 GiB de RAM e eMMC QEMU de 64 GiB, em BIOS e UEFI.
Esses testes não substituem o reteste no HP Stream físico.

Se o disco não aparecer, iniciar o modo live e executar `storage`: o comando
agora mostra fase, CMD, status, present-state, capabilities, versão SDHCI e
ID PCI da falha. Sem host PCI, percorre o namespace ACPI com uACPI e avalia `_HID`, `_UID`,
`_STA`, `_INI`, `_PR0`/`_ON`, `_PS0` e `_CRS` dos dispositivos compatíveis.
Não há varredura especulativa de endereços MMIO. Falhas de AML/energia e
recursos ambíguos ou em RAM impedem publicar o dispositivo.

Referências: [IDs e hosts eMMC Intel no Linux](https://github.com/torvalds/linux/blob/master/drivers/mmc/host/sdhci-pci-core.c),
[divisores e sinalização SDHCI](https://github.com/torvalds/linux/blob/master/drivers/mmc/host/sdhci.c).

### Matriz de simulação completa desta etapa

```sh
make test-storage-matrix
```

O comando compila o QEMU **10.2.0 fixado por SHA-256**, aplica o fixture em
`tests/fixtures/storage/qemu-10.2.0-acpi-emmc.patch` e executa os nove perfis:
IDE, AHCI, NVMe, USB EHCI/BOT, SDHC, eMMC PCI, eMMC ACPI/MMIO, MegaRAID
SAS1078 e SAS2108. Cada perfil exige leitura física, instalação completa de
66 MiB com escrita/leitura de retorno/flush e início da ISO por UEFI com o
mesmo tipo de armazenamento. Também injeta `EIO` na escrita de LBA1 em cada
backend e exige que o instalador aborte sem confirmar a assinatura de boot.
Não há SKIP aceito na matriz estrita.

O cenário ACPI cria um SDHCI **System Bus**, sem função PCI, em `0xfed80000`;
a SSDT é a única descoberta disponível. Usa clock base de 200 MHz, alimentação
de 1,8 V, sem pinos CD/WP e exige `_PR0`/`_ON`/`_PS0` para entregar `_CRS`.
O fixture QEMU só é habilitado por `LUNA_ACPI_STORAGE_FIXTURE=1` e não faz
parte da imagem LunaOS. Também completa o modelo MMC com a faixa OCR de
1,7–1,95 V, ausente no modelo original. O guest não conhece esse endereço
fixo: ele o obtém exclusivamente dos recursos ACPI.

Há casos negativos para xHCI sem driver, NVMe 4Kn, slot SD vazio, `_STA`
ausente, UID de SDIO, recurso sobre RAM e sequência de energia incompleta.
Mídias USB e MegaRAID protegidas são excluídas da instalação. Os testes UEFI
positivos pedem ao OVMF MMIO abaixo de 4 GiB (`X-PciMmio64Mb=0`), limite do
kernel i386 sem PAE. Um conjunto negativo separado mantém a configuração
padrão OVMF e verifica por QMP que BARs NVMe/MegaRAID foram alocados acima
de 4 GiB e corretamente rejeitados, sem truncar endereços ou escrever dados. Esses passes
validam **rejeição**, não suporte a xHCI/4Kn.

Os processos QEMU têm diretórios e sockets separados em `build/storage-matrix`.
As imagens de destino são descartáveis; nenhum caminho `/dev/...` é usado.
Após fechar QEMU, o teste compara todo o payload imutável da imagem e verifica
que os snapshots persistidos contêm a configuração de instalação. Os relatórios
são `dist/storage-matrix-report.json` e `.md`, com hashes da ISO/kernel,
versão do emulador, resultado de cada fase e caminhos dos logs.

Limites: suporte real ainda depende do hardware. Hosts com energia por EC/GPIO
sem handler, Intel RST/VMD, UAS/xHCI, 4Kn e protocolos não implementados não
se tornam funcionais por causa desses testes. ACPI funciona em boot-time,
com SystemMemory/SystemIO/PCI segmento 0; SCI/GPE, hotplug e suspensão não
estão habilitados. Boot pelo firmware no destino e falta de energia permanecem
validações distintas da correção dos comandos de bloco.

Para retomar uma execução parcial do **mesmo** kernel/ISO/fixture:
`LUNA_STORAGE_RESUME=1 python3 tests/storage_matrix.py`. A retomada exige
hashes iguais, preserva o relatório inicial e reaproveita somente fases
concluídas; UEFI e rejeições são executados novamente. O conjunto EIO pode
ser repetido isoladamente com `python3 tests/storage_fault_matrix.py`.
