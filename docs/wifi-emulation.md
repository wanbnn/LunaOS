# Laboratório de dispositivos Wi-Fi

O laboratório usa o QEMU 10.2.0 fixado pelo projeto. O driver nativo do
RTL8822BE acessa BAR2, registradores PCIe e descritores TXBD/RXBD. O ponto de
acesso separado troca quadros 802.11 por UDP local. Os modelos ficam no
ambiente de desenvolvimento.

## Cobertura atual

| Alvo | Modelo | Integração |
|---|---|---|
| RTL8822BE | Energia, efuse de teste, sintonia, transferência de firmware, TX DMA MGMT/BE e RX DMA | Driver nativo, autenticação, associação, WPA2/CCMP, DHCP, DNS, ICMP, HTTP e CSS |
| Intel 8265 | Pendente | Transporte e firmware pendentes |
| Qualcomm QCA9377 | Pendente | BMI, CE, HTC, WMI/HTT e board data pendentes |

Intel AX211/CNVio, Intel 8265/PCIe e variantes Qualcomm requerem modelos e
validação específicos para seus contratos de hardware.

## Executar

Instale as dependências de desenvolvimento no ambiente Python utilizado pelo
projeto e execute o alvo de integração.

```sh
python3 -m pip install -r requirements-dev.txt
make test-wifi-qemu
```

Esse alvo constrói a imagem e o QEMU de laboratório, depois executa os três
scripts de teste. A primeira construção pode baixar dependências para `.tools`.

```sh
python3 tests/wifi_qtest.py
python3 tests/wifi_qemu.py
python3 tests/wifi_join_qemu.py
```

`wifi_qtest.py` verifica PCI/MMIO/DMA independentemente do driver LunaOS,
incluindo PCI BME, checksum do descritor, BCN_VALID W1C e rejeição de firmware
corrompido. `wifi_qemu.py` verifica beacons em duas bandas, ausência de AP e
timeout de firmware com o driver dentro do QEMU.

`wifi_join_qemu.py` opera a página Network por mouse e teclado. A placa
Ethernet permanece desabilitada durante todo o teste. O cenário cobre recusa
de autenticação, senha incorreta, nova tentativa, handshake WPA2, CCMP, DHCP,
ARP, DNS, ping, HTTP, stylesheet completo de 350 KB e continuidade de ping.
Também injeta uma retransmissão criptografada e uma desconexão pelo AP, seguida
de reconexão.

A criptografia do AP usa implementações independentes de hashlib/hmac e
cryptography. SSID, senha e serviços determinísticos ficam no fixture Python.
Os arquivos de produção usam suas APIs e o driver PCIe normais.

## Arquivos e configuração

- `tests/fixtures/wifi/rtw8822be.c` implementa o dispositivo PCIe de laboratório.
- `tests/fixtures/wifi/access_point.py` implementa o AP independente.
- `tools/wifi/build-qemu.py` incorpora o dispositivo à árvore QEMU fixada.
- `build/wifi-qtest/` contém os resultados dos testes de registradores.
- `build/wifi-qemu/` contém relatórios e logs dos cenários de scan e firmware.
- `build/wifi-join-qemu/` contém `report.json`, `serial.log` e `peer.json` da conexão.

O dispositivo expõe as propriedades a seguir.

- `firmware=CAMINHO` seleciona os bytes de referência comparados durante a
  transferência por TXBD/reserved page e IDDMA.
- `capture=CAMINHO` reproduz PCAP clássico little endian, DLT_IEEE802_11 105,
  contendo beacons sem radiotap e sem FCS.
- `stall-firmware=on` injeta timeout de boot.
- `air-port=PORTA` seleciona o AP em UDP no endereço loopback. Os datagramas
  contêm um byte de canal e um quadro 802.11. Um datagrama com apenas o canal
  solicita um beacon.

O modelo usa vendor/device `10ec:b822` e subsystem de laboratório `1b36:8822`.
A efuse contém RFE e MAC de teste. Os contratos de registradores e descritores
partem das fontes Linux rtw88 usadas pelo projeto.

## Validação física pendente

A comparação dos bytes transferidos do firmware valida o transporte e o
protocolo de boot modelado. Execução da CPU do firmware, RF analógico,
calibração, interferência e temporização física dos ACKs permanecem pendentes.
Os testes virtuais verificam o percurso de software e DMA modelado. A
estabilidade do RTL8822BE físico com o MitraStar GPT-2741GNAC-N2 precisa de
validação no notebook.

Registradores além dos contratos implementados usam armazenamento simples.
A falha física que motivou a restauração da configuração de associação
também aguarda confirmação no notebook.
