<div align="center">

# LunaOS

**Um sistema operacional independente, desenvolvido do zero em C e Assembly para x86.**

![Status](https://img.shields.io/badge/status-experimental-F59E0B?style=flat-square)
![Versão](https://img.shields.io/badge/vers%C3%A3o-0.3-6366F1?style=flat-square)
![Kernel](https://img.shields.io/badge/kernel-i386-334155?style=flat-square)
![Boot](https://img.shields.io/badge/boot-BIOS%20%7C%20UEFI-0F766E?style=flat-square)
![Linguagens](https://img.shields.io/badge/c%C3%B3digo-C%20%2B%20Assembly-2563EB?style=flat-square)

**[Visão geral](#visão-geral) · [Funcionalidades](#funcionalidades-e-limitações) · [Executar](#começando) · [Arquitetura](#arquitetura) · [Testes](#testes-e-validação) · [Documentação](#documentação)**

</div>

> [!IMPORTANT]
> **O LunaOS está em desenvolvimento.** Este repositório contém implementações reais e recursos experimentais, não um sistema pronto para substituir Linux ou Windows. Testes em QEMU, simulações de rádio e compilação bem-sucedida **não** comprovam compatibilidade com hardware físico. A situação descrita abaixo corresponde ao código da branch `main` em **7 de novembro de 2026**.

## Visão geral

O LunaOS tem **bootloader próprio**, **kernel próprio em C**, **interface gráfica nativa** e **aplicativos e serviços integrados**. O projeto não usa o kernel Linux, GRUB ou Bun para executar o sistema. Código de bibliotecas e fontes de drivers de terceiros é incorporado ou usado como referência com suas respectivas licenças — isso **não** oferece compatibilidade binária com módulos Linux.

**O que existe hoje:**

- Boot BIOS x86 e caminho **UEFI x64 experimental** que transfere o controle ao kernel i386.
- Desktop com dock, pesquisa, janelas, compositor 32 bits, **transições suaves nativas**, fontes **Inter Tight** e ícones **Lucide**.
- Assistente de primeiro boot, modo live, instalação em disco IDE compatível e login.
- Terminal **lunaSH**, Files, Notes, Settings, navegador e Gerenciador de Tarefas.
- **LunaSDK v2**: aplicativos JavaScript `.ljs`, Python limitado `.py` (prévia), API de descoberta **PCI real** e endpoints de hardware implementados por drivers C nativos; projetos C/CMake ainda são compilados no host.
- Pilha de rede IPv4 via **RTL8139**, HTTP e HTTPS/TLS 1.2.
- Navegador com HTML, CSS, DOM/JavaScript **parciais** e decodificação de PNG externo.
- Infraestrutura de drivers e desenvolvimento de Wi-Fi com scanner passivo experimental para **RTL8822BE**.
- Gerenciamento dinâmico de memória física e contabilidade de RAM por processo.
- Calendário mensal, configuração de data e hora offline e sincronização **SNTP/NTP**.

A geração atual produz `dist/LunaOS.img` (disco raw de **32 MiB**) e `dist/LunaOS.iso` (CD inicializável BIOS de aproximadamente **32,125 MiB**). O limite de **120 MB** continua sendo um objetivo do projeto, não uma garantia para futuras integrações.

## Funcionalidades e limitações

| Componente | Estado | Detalhes |
| --- | --- | --- |
| **Kernel e processos** | Implementado, com limites | Kernel i386, separação de privilégios, PIDs, tarefas em ring 3 e serviços centrais em ring 0; sem loader ELF de uso geral nem isolamento completo dos serviços |
| **Boot BIOS** | Implementado | Bootloader próprio; carregamento do kernel acima de 1 MiB |
| **Boot UEFI** | Experimental | Loader UEFI x64 com transição para kernel i386; caminho testável com OVMF/QEMU |
| **Desktop** | Implementado | Janelas, minimização, maximização, busca, atalhos, compositor com meta de 60 fps e resoluções do adaptador virtual até 1920×1080 |
| **Sistema de arquivos** | Limitado | LunaFS com snapshots, até 64 entradas e arquivos de até 8191 bytes |
| **Instalador** | Implementado para IDE | Seleção explícita de disco, confirmação destrutiva, cópia verificada, usuário e senha; sem AHCI, NVMe ou USB mass storage |
| **Ethernet e HTTPS** | Parcial | RTL8139, IPv4, DNS, TCP, HTTP e BearSSL/TLS 1.2; não é uma stack de rede universal |
| **Navegador nativo** | Experimental | HTML e CSS com layout/cascata parciais, bindings DOM/JS e PNG; **sem** compatibilidade moderna completa |
| **Wi-Fi RTL8822BE** | **Scanner passivo experimental** | Inicialização PCIe, firmware, PHY e recebimento de beacons por DMA; **sem conexão**, ainda sem validação em placa física |
| **Wi-Fi Intel AX211** | Infraestrutura | Identificação e diagnóstico PCI/firmware; **sem scan físico nem conexão** |
| **Camadas Wi-Fi comuns** | Testadas em simulação | Interfaces de scan/connect, handshake WPA2-PSK, porta controlada e DHCP; dependem de um rádio físico funcional |
| **Memória e Task Manager** | Implementado, com limites | Páginas físicas, heap/DMA sob demanda, RAM por PID; i386 sem PAE não aloca RAM acima de 4 GiB |
| **Calendário e relógio** | Implementado | Ajuste manual, offset UTC, RTC, persistência e SNTP assíncrono; sem base IANA de fusos/DST |
| **Bluetooth e hardware geral** | Pendente | Sem pilha Bluetooth, USB geral e drivers universais de GPU, armazenamento ou Wi-Fi |

### Navegador: evolução real, sem prometer um Chrome

O navegador deixou de ser apenas um leitor de texto HTML. O código nativo em `ui/web_*.c` implementa parsing HTML, CSS com cascata e layout parciais, alguns bindings DOM/JavaScript e renderização de imagens **PNG externas** com limites de memória e dimensões.

Entre os avanços recentes estão margens e dimensões CSS, regras `@media`, floats, posicionamento básico, `overflow`/clipping, `box-sizing`, especificidade da cascata e `!important`. Há testes de regressão do motor no host.

**O que isso não significa:** WebKit não foi integrado; Flexbox/Grid completos, Web APIs modernas, scripts externos de sites e compatibilidade geral com a web atual **não estão concluídos**. A execução isolada de QuickJS também não equivale a um browser completo.

Detalhes técnicos: [Motor do navegador](docs/browser-engine.md).

### Wi-Fi: o que está funcionando no código

A API nativa de Wi-Fi permite que o setup consulte drivers registrados e apresente **apenas SSIDs realmente recebidos**, sem redes falsas. O código inclui frames IEEE 802.11, diagnóstico PCI/firmware e componentes compartilhados de **WPA2-PSK/CCMP** e **DHCP** validados em testes controlados.

**RTL8822BE (`10ec:b822`)**

- Caminho experimental de inicialização PCIe, firmware, energia e PHY, com anéis DMA.
- Scanner **passivo** para canais de 2,4 e 5 GHz, com leitura de beacons/probe responses e RSSI.
- Diagnóstico de etapas e erros pelo setup e pelo comando `wifidiag`.
- **Ainda não validado em hardware físico.** O backend não oferece autenticação, associação, WPA2 conectado ao rádio ou tráfego IP. O botão de conexão permanece indisponível.

**Intel AX211 (`8086:51f0`)**

- Inventário do controlador, preparação de PCI/MMIO e parser/validação de firmware Intel.
- Integração compartilhada de WPA2, DHCP e interface de rede **testada com rádio simulado**.
- **Ainda sem boot do firmware no dispositivo, scan físico ou conexão.**

A identificação de outros chipsets Realtek, MediaTek, Qualcomm e Broadcom é **inventário/preflight**, não uma coleção de drivers prontos. As fontes Linux arquivadas no projeto não são executadas como drivers Linux dentro do LunaOS.

Leia [RTL8822BE nativo](docs/rtw8822be-native-port.md), [Intel AX211](docs/intel-ax211-wifi.md), [Porte de Wi-Fi](docs/wireless-port.md) e [Serviços para drivers](docs/driver-services.md).

### Memória, processos e relógio

- O alocador físico usa o mapa de memória recebido via BIOS E820 ou pela tradução UEFI. Heap e buffers DMA crescem conforme páginas disponíveis, substituindo pools fixos anteriores.
- O comando `free` mostra reservas, páginas alocadas e livres; `memtest` verifica alocação e liberação. O kernel continua **i386 sem PAE**, com o limite de endereçamento correspondente.
- `ps` e o Gerenciador de Tarefas mostram **RAM por PID**, incluindo arenas, pilhas e estruturas de paginação. A contabilização não transforma os backends de aplicativos em processos completamente isolados.
- O relógio da barra superior abre um **calendário mensal**. Em **Settings > Date & time** é possível definir data/hora sem internet, ajustar offset UTC e configurar sincronização SNTP assíncrona (servidor padrão: `time.cloudflare.com`). Não há banco de fusos IANA nem autenticação NTS.

Detalhes do motor de animações: [Motion Engine](docs/motion.md).

Saiba mais: [Memória física](docs/physical-memory.md), [RAM por processo](docs/process-memory.md) e [Calendário e horário](docs/calendar.md).

## Começando

### Requisitos de desenvolvimento

Host Linux x86 com ferramentas GCC/binutils e **suporte de compilação de 32 bits**, Make, Python 3, Clang/LLD (loader UEFI) e QEMU x86. Os utilitários Python para a **build**, como Pillow e CairoSVG, estão em `requirements-dev.txt`. O script `tools/bootstrap.sh` pode instalá-los localmente em `.tools/python`; ele verifica as bibliotecas e os cabeçalhos de compilação 32 bits sem precisar de `ripgrep`.

Exemplo no Ubuntu (nomes de pacotes sujeitos à versão da distribuição):

```bash
sudo apt update
sudo apt install -y build-essential gcc-multilib libc6-dev-i386 binutils clang lld \
  make python3 python3-pip qemu-system-x86 ovmf
```

```bash
git clone https://github.com/wanbnn/LunaOS.git
cd LunaOS
bash tools/bootstrap.sh
make -j2 all
```

A build gera as imagens em `dist/`. As fontes incorporadas de musl, QuickJS e BearSSL já estão fixadas no repositório; o processo normal de compilação não depende de baixar esses códigos novamente.

### Executar em QEMU

**BIOS / imagem de disco (padrão):**

```bash
make run
```

**Boot pelo CD ISO (BIOS/El Torito):**

```bash
make run-iso
```

**UEFI experimental com OVMF:**

```bash
LUNA_UEFI=1 make run
```

O launcher inicia, por padrão, com **64 MiB de RAM em BIOS** e **256 MiB em UEFI**. Para alterar a RAM da VM:

```bash
LUNA_RAM=256 make run
```

O QEMU usa KVM se estiver disponível, caso contrário usa emulação TCG. O launcher configura o adaptador gráfico virtual, Ethernet RTL8139, áudio AC97 e mídia IDE. O modo UEFI exige OVMF instalado/configurado. Para detalhes sobre boot e mídia, consulte [Arquitetura](docs/architecture.md) e [ISO](docs/iso.md).

> [!CAUTION]
> **Não use um disco físico com o instalador experimental.** A instalação formata e substitui o conteúdo do destino selecionado. Para testes, crie uma **imagem de disco descartável** e siga o procedimento documentado em [Primeiro boot e instalação](docs/setup.md). A mídia live também permite explorar o desktop sem instalar.

## Uso básico

**Atalhos:**

| Atalho | Ação |
| --- | --- |
| `Super` / tecla Windows | Abrir a busca de aplicativos |
| `Alt+Tab` e `Alt+Shift+Tab` | Alternar entre janelas |
| `Alt+F4` | Fechar a janela ativa |
| `Ctrl+Alt+T` | Abrir o lunaSH |
| `Ctrl+Shift+Esc` | Abrir o Gerenciador de Tarefas |

O terminal **lunaSH** oferece histórico, autocompletar, pipes de texto, redirecionamentos e scripts `.lsh`. Ele **não** é um shell POSIX completo e não executa binários Linux externos.

Comandos úteis no guest:

```sh
help
ps
free
net
wifidiag
wifidiag intel
date
date zone -03:00
date sync
memtest 4
sync
```

O app **Notes** oferece edição de textos e realce de sintaxe para alguns formatos. **Settings** controla aparência, teclado, mouse, monitor virtual, áudio, idioma e relógio. Nem todos os controles representam capacidades do hardware físico.

### Criar seus próprios aplicativos e dispositivos virtuais

O comando **`dev`** permite editar código dentro do LunaOS com **Notes**, executar scripts `.ljs` com QuickJS e executar uma **prévia limitada** de arquivos `.py`. A API `luna.hardware.pci` enumera dispositivos PCI físicos e lê a área de configuração com validação; drivers C nativos podem registrar endpoints que comunicam com hardware real (o exemplo `qemu-edu` usa PCI/MMIO):

```sh
dev new hello js
dev run /home/luna/hello.ljs
dev new probe py
dev run /home/luna/probe.py
dev hardware pci
dev hardware read 0 2 0 0 4
dev driver list
dev new native_probe c
dev list
```

Os caminhos acima usam o usuário live padrão `luna`; adapte-os ao diretório `/home/SEU_USUARIO` após a instalação. **Drivers C reais usam `kernel/driver.h` para PCI, MMIO, DMA e IRQ e podem publicar um endpoint LunaSDK de leitura/escrita validada**. O exemplo `qemu-edu` depende de um dispositivo QEMU EDU realmente conectado (`tools/run-qemu.sh -device edu`). **CMake ainda roda no host**, já que LunaOS não tem compilador C nativo nem loader ELF; a prévia `.py` aceita somente um subconjunto pequeno da sintaxe Python e não substitui MicroPython/CPython. Scripts não confiáveis ainda não estão isolados do kernel.

Contrato, exemplos, limites e teste: [API para desenvolvedores](docs/developer-api.md).

## Arquitetura

```text
LunaOS/
├── boot/          # Bootloaders BIOS e UEFI
├── kernel/        # Kernel, tarefas, memória, serviços e relógio
├── drivers/       # PCI, IDE, vídeo/entrada, áudio, Ethernet e Wi-Fi experimental
├── net/           # Protocolos de rede e componentes 802.11/WPA2/DHCP/NTP
├── ui/            # Desktop, janelas, setup e motor web nativo
├── apps/          # Shell, JavaScript e aplicativos
├── lib/           # Runtime e utilitários nativos
├── assets/        # Fontes, ícones, firmware e materiais licenciados
├── third_party/   # Fontes de bibliotecas de terceiros com licenças
├── tools/         # Geração de imagens, build e execução
├── tests/         # Testes unitários, host e integração em guest/QEMU
├── docs/          # Documentação técnica
└── Makefile
```

O kernel utiliza contextos de tarefas e proteção de páginas para clientes ring 3, mas diversos backends continuam executando como serviços do kernel. **Não** há ABI genérica para aplicativos Linux, loader ELF geral, threads POSIX completas ou isolamento de segurança equivalente ao de um sistema operacional de produção.

## Testes e validação

```bash
make check                  # Imagem, ISO e testes host previstos no target
make test                   # Testes adicionais com guest/QEMU
make test-webengine         # Regressão HTML/CSS/layout/PNG no host
make test-wifi-station      # WPA2 + DHCP integrados com rádio simulado
make test-rtw8822be-rx-dma  # Modelo dos descritores RX do RTL8822BE
make test-pmm test-heap     # Gerenciamento de memória
```

Testes específicos de calendário, UEFI, instalação e drivers estão descritos nas páginas técnicas. A pipeline [Wireless portability smoke tests](.github/workflows/wireless-smoke.yml) faz verificações de portas Wi-Fi e build da imagem, mas **não testa um adaptador Wi-Fi físico**.

**Interpretação correta dos testes:**
- Compilar código freestanding ou passar testes com PCI/rádio simulado **não** comprova que o firmware inicializou em uma placa real.
- Detectar um SSID via scan passivo **não** comprova associação, autenticação WPA2, DHCP ou conectividade.
- Renderizar uma página de exemplo **não** comprova conformidade com a web moderna.
- Testes no QEMU **não** comprovam suporte a controladores reais não implementados.

## Documentação

| Tema | Referência |
| --- | --- |
| Organização interna e proteções | [Arquitetura](docs/architecture.md) |
| Primeiro boot, login e instalação | [Setup](docs/setup.md) |
| ISO/ATAPI e mídia inicializável | [ISO](docs/iso.md) |
| Capacidades e limites do navegador | [Motor do navegador](docs/browser-engine.md) |
| API de drivers DMA, PCI, MMIO e firmware | [Serviços de drivers](docs/driver-services.md) |
| Desenvolvimento in-guest, integração com PCI real e CMake no host | [LunaSDK v2](docs/developer-api.md) |
| Famílias e estratégia de drivers Wi-Fi | [Porte de Wi-Fi](docs/wireless-port.md) |
| RTL8822BE experimental | [Port nativo RTL8822BE](docs/rtw8822be-native-port.md) |
| Intel AX211 e WPA2/DHCP | [Intel AX211](docs/intel-ax211-wifi.md) |
| Alocação física e limites de RAM | [Memória física](docs/physical-memory.md) |
| Contabilidade por processo | [Memória por processo](docs/process-memory.md) |
| RTC, calendário e NTP | [Calendário](docs/calendar.md) |

## Próximos desafios

O roadmap imediato continua sendo **validar os drivers em hardware físico**, completar autenticação/associação e tráfego IP Wi-Fi, ampliar suporte de armazenamento e periféricos, fortalecer isolamento e APIs de processos, e avançar na compatibilidade do navegador. Essas metas **não devem ser confundidas com funcionalidades já entregues**.

## Licenças e créditos

O LunaOS utiliza componentes de terceiros, incluindo **musl**, **QuickJS**, **BearSSL**, **stb_image**, **Inter Tight**, **Lucide** e materiais upstream de drivers/firmware. Cada componente mantém seus avisos e condições de redistribuição. Consulte [third_party/README.md](third_party/README.md), os avisos em `assets/licenses/` e `/etc/licenses` no sistema.

---

<div align="center">

**LunaOS · Construindo um sistema operacional, um componente de cada vez.**

</div>
