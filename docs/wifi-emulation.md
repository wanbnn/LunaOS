# Laboratório de dispositivos Wi-Fi

O laboratório usa modelos explícitos de componentes dentro do QEMU 10.2.0.
Não substitui as interfaces de chipset por uma placa Ethernet. Os modelos são
ferramentas de desenvolvimento e não são incluídos na ISO.

## Estado verificável

| Alvo | Modelo QEMU | Driver LunaOS | Conexão |
|---|---|---|---|
| Realtek RTL8822BE | Parcial: energia, efuse de teste, RF, transferência de firmware e RX DMA | Driver nativo existente exercitado no QEMU, scanner passivo | Não implementada |
| Intel 8265 | Pendente | Transporte 8000, firmware ALIVE, NVM e filas pendentes | Não implementada |
| Qualcomm QCA9377 | Pendente | BMI, CE, HTC, WMI/HTT, firmware e board data pendentes | Não implementada |

Esta etapa não completa o pedido de suporte funcional dos três fabricantes.
Intel AX211/CNVio não é equivalente a Intel 8265/PCIe. O modelo do 8265 não
validará o AX211. Também não se deve extrapolar entre variantes Qualcomm que
compartilham IDs PCI.

## Construir e executar

```sh
make -j4
python3 tools/wifi/build-qemu.py
python3 tests/wifi_qtest.py
python3 tests/wifi_qemu.py
```

O builder reutiliza a árvore QEMU fixada por `tools/storage/build-qemu.py`.
Na primeira execução pode baixar o QEMU e dependências de compilação para
`.tools`. Adiciona `lab-rtw8822be`, sem alterar os dispositivos Ethernet.

O teste de integração usa cópias da imagem em `build/wifi-qemu/`, desabilita
placas Ethernet e cria um PCAP com dois APs explicitamente sintéticos. O driver
lê os beacons através de descritores RX reais da família rtw88, faz a sintonia
e publica os resultados na API Wi-Fi normal. SSIDs não estão embutidos no
driver nem na imagem do sistema.

No terminal do LunaOS:

```text
wifidiag
wifi status
wifi scan
wifi results
```

A varredura é assíncrona. Aguarde `wifi status` sair de `link=1` antes de ler
os resultados. O backend passivo informa `connect=0`.

## Modelo RTL8822BE

Fonte: `tests/fixtures/wifi/rtw8822be.c`, GPL-2.0-or-later, compilada apenas no
QEMU de laboratório. Contratos consultados: fontes upstream Linux v6.12:

- https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/realtek/rtw88/pci.c
- https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/realtek/rtw88/rx.h
- https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/realtek/rtw88/fw.c
- https://github.com/torvalds/linux/blob/v6.12/drivers/net/wireless/realtek/rtw88/rtw8822b.c

Propriedades QEMU:

- `firmware=CAMINHO`: referência obrigatória do firmware RTL8822B. Cada bloco
  recebido por TXBD/reserved page e IDDMA precisa coincidir com a referência.
  O estado de boot só é publicado após receber todos os segmentos.
- `capture=CAMINHO`: opcional, PCAP clássico little endian, versão 2.4,
  DLT_IEEE802_11 (105), quadros sem radiotap e sem FCS. Reproduz beacons do
  canal sintonizado, com FCS reservado no pacote RX e PHY RSSI de teste.
- `stall-firmware=on`: impede a conclusão do boot para verificar timeout e
  limpeza de recursos no driver.

O modelo identifica-se com vendor/device `10ec:b822` e subsystem de laboratório
`1b36:8822`. Efuse/RFE e RSSI são valores do laboratório, não um dump de placa.
Os bytes de firmware são comparados, **não executados** em uma CPU emulada.
Não há TX de dados, MLME, criptografia CCMP, MCU ISA, interrupções, economia de
energia, migração ou reprodução analógica de RF. Registradores não modelados
usam armazenamento simples; portanto o resultado não demonstra conformidade
com todo o hardware. Não foi validado com um driver Linux independente.

`wifi_qtest.py` programa PCI, MMIO e descritores sem linkar o driver LunaOS:
verifica identidade, leitura efuse, sintonia RF, bloqueio por PCI BME, checksum
do TX descriptor, BCN_VALID W1C e rejeição de firmware incorreto/incompleto.
`wifi_qemu.py` executa o driver nativo em três cenários: beacons em 2 bandas,
ar silencioso e timeout de boot. Relatório e logs ficam em `build/wifi-qemu/`.

## Próximos contratos necessários

O RTL precisa de TXBD de dados e completion, autenticação/associação 802.11,
programação CAM/CCMP, calibração e limites regulatórios antes de ser considerado
um driver de conexão. O núcleo WPA2/IP existente só deve receber a interface
após uma associação verificada; Ethernet funcional não prova associação Wi-Fi.

Intel 8265 precisa do transporte PCIe da família 8000, CSR/FH/PRPH, carga de
microcódigo, ALIVE, NVM e comandos de scan/MLME. QCA9377 precisa dos registradores
CE corretos da revisão, BMI, HTC e protocolos WMI/HTT e board data. Esses modelos
não devem ser implementados como dispositivos genéricos com IDs de fabricantes.

Testes virtuais não certificam operação em placas físicas. Passar os testes
acima não habilita Wi-Fi de conexão na ISO.
