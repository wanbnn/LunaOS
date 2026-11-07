# LunaOS 0.3 — sistema independente em desenvolvimento

Bootloader próprio em assembly x86, kernel próprio em C, interface nativa com Inter
Tight e ícones exclusivamente Lucide. A imagem não usa kernel Linux, GRUB ou Bun.
Há bibliotecas incorporadas de criptografia, JavaScript e funções ISO-C; seus avisos
estão em `third_party/README.md` e em `/etc/licenses` dentro do guest.

**Imagens:** `dist/LunaOS.img`, disco BIOS raw de **32 MiB**, e
`dist/LunaOS.iso`, CD BIOS inicializável de **32,125 MiB**. O limite de 120 MB continua
sendo uma exigência do projeto. **DOM/CSS modernos ainda não estão integrados.**

![Área de trabalho](dist/desktop-v03.png)

## Abrir no QEMU

```bash
cd /home/wanderson/Projetos/LunaOS
make run
# Ou iniciar pelo CD:
make run-iso
```

Feche uma instância antiga para iniciar a imagem nova. O launcher usa 64 MiB de RAM por padrão de teste (configure, por exemplo, `LUNA_RAM=4096 make run`),
VGA padrão com modo nativo de 32 bits, IDE, RTL8139 e AC97. Usa KVM e CPU host quando
`/dev/kvm` está acessível; caso contrário usa TCG e CPU max. O guest desta versão
requer **64 MiB**, por causa dos caches gráficos e reservas do runtime.

O QEMU continua aparecendo como um único processo no computador hospedeiro. Os PIDs
dos aplicativos são administrados dentro do LunaOS.

```bash
tools/run-qemu.sh -display none           # logs seriais; setup requer GUI/QMP
LUNA_AUDIO=driver=alsa,id=audio0 make run  # host com backend ALSA disponível
```

`Ctrl esquerdo + Alt esquerdo` libera a captura de mouse do QEMU.

## Atalhos e terminal

- **Super / Windows:** busca de aplicativos no topo da tela. Digite, use setas e Enter.
- **Alt+Tab / Alt+Shift+Tab:** alternar entre janelas, inclusive minimizadas.
- **Alt+F4:** fechar a janela em foco.
- **Ctrl+Alt+T:** abrir o terminal.
- **Ctrl+Shift+Esc:** abrir o Gerenciador de Tarefas.
- **Esc:** fechar busca ou painel rápido; não minimiza o aplicativo automaticamente.
- Os atalhos globais **F1–F6 foram removidos**.

No lunaSH, Tab completa comandos e caminhos. Em uma correspondência única, insere
um espaço ou `/` para diretórios. Duas pressões de Tab exibem alternativas quando
não há mais prefixo comum. Há edição com setas, Home, End e Delete, histórico com
setas, Ctrl+A/E, Ctrl+C e Ctrl+L. Não existe job control Unix: Ctrl+C cancela a linha
em edição, sem ser uma implementação geral de sinais para programas externos.

```sh
help
mkdir projetos
cd projetos
echo "ola Luna" > notas.txt
cat notas.txt | grep Luna | wc
ps
tasks
ringtest
sync
```

Aspas, variáveis, pipes de texto sequenciais, redirecionamentos e scripts `.lsh`
continuam disponíveis. Não há execução de binários externos, globbing, expressões
regulares ou compatibilidade POSIX completa. A conclusão inicial cobre nomes simples;
nomes com espaços/escapes complexos ainda têm limitações.

## Janelas e configurações

Arraste a barra de título para mover. Vermelho fecha, amarelo minimiza e verde
maximiza/restaura; duplo clique na barra alterna maximização. Minimizar mantém o PID;
fechar termina o cliente; reabrir cria um novo PID.

O ícone de sliders na barra superior abre o **painel rápido**, mantido separado do
aplicativo **Settings** disponível pelo dock ou busca Super.

Settings inclui:

- Teclado: layout US e mapeamento ABNT2 básico, atraso e taxa de repetição PS/2.
- Mouse: velocidade linear e botões para mão esquerda. A aceleração PS/2 é desativada.
- Monitores: modo atual, meta do compositor e limites reais do driver.
- Som: volume, balanço, mute e teste no AC97. Não há entrada de microfone/USB audio.
- Aparência: vidro escuro, seis acentos, quatro fundos e carregamento de PPM P3.
- Rede: estado real do RTL8139; Wi-Fi e Bluetooth continuam não implementados.

As alterações são imediatas. **Save settings** salva `/etc/luna-settings` e faz sync
do LunaFS. O botão também persiste arquivos pendentes, pois o snapshot é do volume.

Para fundo próprio, o formato inicial é **PPM P3 de texto**, até 64×64 e dentro do
limite de 8191 bytes por arquivo. A imagem é ampliada com interpolação bilinear.
Não existe ainda importação PNG/JPEG ou seletor de arquivos gráfico completo.

## Renderização e 60 fps

O compositor tem meta de **60 fps**, com pacing fracionário sobre timer de 1 kHz;
não usa o arredondamento fixo de 17 ms que limitaria a cadência a 58,8 fps.
As janelas têm superfícies em cache. O cursor restaura/desenha somente sua região,
sem redesenhar todo o desktop. Scanout de 32 bits usa cópias em bloco.

`bench` mede a capacidade de composição de 120 frames com as janelas abertas. Isso
é throughput, não uma promessa de que qualquer rotina do kernel, máquina hospedeira
ou futuro conteúdo web sempre cumpra o orçamento de frame.

**A frequência física do monitor não é programável por este driver.** A resolução
do framebuffer pode variar de 1024×768 a 1920×1080. O painel não oferece 120/144 Hz fictícios: taxas superiores dependem de
um driver que consulte e programe timings reais do display. O QEMU/host controla a
saída física. Em TCG ou no fallback VBE, 60 fps não é garantido.

## Navegador: estado explícito

O navegador atual é um **leitor nativo de texto HTML e HTTPS**. Não executa scripts
de páginas e não tem DOM, CSSOM, layout CSS moderno ou APIs completas de navegador.
O requisito de navegar na web moderna foi aceito, mas **não foi concluído**.

A avaliação do porte de WebKit, dependências e critérios de aceitação está em
[docs/browser-engine.md](docs/browser-engine.md). O fixture
`tests/web-platform/modern.html` ainda não foi aprovado pelo guest. Rodar JavaScript
no terminal ou abrir essa página em um browser do host não comprova DOM/CSS no LunaOS.

Funciona nesta versão:

- HTTPS TLS 1.2 com BearSSL, cifras AEAD/ECDHE, cadeia, nome e data verificados.
- Entropia de RDRAND: se indisponível, TLS falha; não há fallback inseguro de entropy.
- 120 âncoras públicas Mozilla, registradas em `assets/tls/manifest.json`.
- CA adicional opcional em `/etc/tls/custom.pem`, instalada explicitamente pelo usuário.
- Transferência chunked e validação do tamanho da resposta; limite de documento de 128 KiB.
- Perfil IPv4 estático QEMU: `10.0.2.15/24`, gateway `.2`, DNS `.3`; `/etc/hosts` é consultado.

Ainda faltam TLS 1.3, HTTP/2, redirecionamentos, compressão, cookies/storage, um motor
web completo e um sandbox de conteúdo de produção. O driver TCP ainda é restrito;
não se trata de uma pilha de rede pronta para todos os ambientes físicos.

```sh
net
ping 10.0.2.2
wget https://servidor/caminho /home/luna/documento.html
browse file:///home/luna/documento.html
js '[1,2,3].map(x => x * 2).join(",")'
```

O comando `js` usa **QuickJS 2026-06-04** com sintaxe moderna/BigInt, limite de memória
e interrupção por orçamento de execução. Ele não é uma camada DOM/CSS nem está
anunciado como o motor final do navegador; WebKit inclui seu próprio JavaScriptCore.

## Privilégios e limites do sistema

| Componente | Estado |
| --- | --- |
| Ring 0 | Kernel, drivers, compositor e serviços essenciais; tarefa protegida |
| Rings 1 e 2 | Contextos e seletores testados, reservados a serviços confiáveis |
| Ring 3 | Clientes dos seis aplicativos, com PID, CR3 e pilhas privados |
| Backends de aplicativos | Lógica de terminal, rede e desenho ainda em serviços do kernel |
| Arquivos | LunaFS, 64 entradas, arquivos de texto de até 8191 bytes, sync explícito |
| Entrada | PS/2 com filas, deltas de mouse de nove bits e hotspot corrigido |
| Dispositivos | IDE ATA, RTL8139, AC97/PIT e VBE/Bochs framebuffer |
| Ausências | UEFI, USB, NVMe/AHCI, Wi-Fi/Bluetooth e suporte geral a hardware |

Não há loader ELF de aplicações gerais, threads POSIX, heap de processo sob demanda
ou IPC completo. `reboot` e `poweroff` requerem capacidade de sistema do ring 0.
O runtime de JS é um serviço confiável do kernel; não execute conteúdo não confiável
como se já houvesse um sandbox de navegador completo.

## Construir e verificar

Host: GCC/binutils com runtime de 32 bits (`gcc-multilib`), Make, Python/Pillow/CairoSVG,
OpenSSL para os testes de TLS, QEMU x86/SeaBIOS e GUI GTK. `tools/bootstrap.sh` instala
apenas ferramentas Python locais. Os fontes de musl, QuickJS e BearSSL são vendorizados;
a compilação normal não acessa a rede.

```bash
make -j4
make check
make test
make run
```

O loader agora lê o kernel em blocos, usando bounce buffer BIOS e cópia para RAM acima
de 1 MiB; não está mais limitado ao espaço abaixo de 640 KiB. A imagem segue raw de
32 MiB. Compilar novamente recria a imagem de distribuição; use cópias para guardar
trabalho importante do guest.

Os relatórios em `dist/test-report.json`, `multitask-report.json`, `desktop-report.json`
e `web-foundation-report.json` identificam o hash exato testado. São separados testes
de infraestrutura TLS/JS e a ausência de um motor DOM/CSS moderno. As capturas PNG
em `dist` vêm do framebuffer real do guest.

### Atualização de desktop e hardware

- Mouse absoluto VMMouse no QEMU, com fallback PS/2 relativo em hardware real.
- Cabeçalho de janela reduzido de 45 para 33 pixels, com hitboxes correspondentes.
- Configurações → Displays: dropdown de modos do adaptador virtual Bochs/QEMU,
  até 1920×1080. A saída e o desktop são redesenhados na resolução escolhida, em escala 100%.
  O modo pode ser salvo. Não há enumeração/troca de modos de monitores físicos
  para GPUs reais; o fallback VBE conserva seu modo de boot.
- Configurações → System: idioma inglês/português para os controles traduzidos e
  modo de desenvolvedor que exibe o tempo de composição. Tradução parcial;
  o shell e vários textos de aplicativos continuam em inglês.
- Notes: app separado no dock, edição de arquivos, cursor, navegação, indentação,
  autoindentação e destaque lexical para Python, Markdown, C/C++, JS e JSON.
  `Ctrl+L` edita o caminho; Enter abre; `Ctrl+S` salva no disco. Arquivos até 8 KiB,
  sem LSP, verificação de tipos ou execução de Python. A escolha de sintaxe fica
  no lado direito da barra. Abrir arquivo substitui o buffer: salve antes.
- Wi-Fi: descoberta PCI de 15 alvos; **nenhum driver Wi-Fi funcional ainda**.
  O porte Linux foi avaliado em [wireless-port.md](docs/wireless-port.md).
  Bluetooth e WebKit continuam pendentes.

`tests/desktop_next.py` verifica dentro do guest os quatro cantos do mouse,
a troca real de scanout, a aba System e criação de arquivo pelo Notes.


### Serviços nativos para portes Wi-Fi

Implementada a [API de serviços de drivers v1](docs/driver-services.md), em
[kernel/driver.h](kernel/driver.h): DMA contíguo com máscara/alinhamento,
PCI/MMIO sem cache, INTx compartilhado via PIC, locks IRQ-save, completions,
work/timers limitados, buffers/filas de pacotes e arquivo binário de firmware
independente do LunaFS. `drvinfo` mostra capacidades; `drvtest` executa diagnóstico.

`tests/driver_services.py` valida no guest DMA bidirecional e interrupções de um
PCI EDU real do QEMU, firmware binário de 70 KB, corrupção e liberação de recursos.
MSI/APIC, IOMMU, compatibilidade mac80211 e rádio Wi-Fi funcional ainda não estão
implementados. Essa camada permite iniciar a adaptação dos drivers; não é a pilha
Wi-Fi completa nem um adaptador binário para módulos Linux.


### Assistente de primeiro boot

A imagem agora tem **32 MiB** e abre um setup nativo com boas-vindas em seis
idiomas, seleção inglês/português e teclado, estado do hardware/rede, região,
usuário/senha, seleção/confirmacão do disco e instalação com verificação e reinício.
A senha é um hash PBKDF2 com salt e é exigida no boot instalado. “Experimentar
LunaOS” abre o desktop live; `setup` no lunaSH reabre o assistente.

Inclui um snapshot completo dos fontes Linux `drivers/net/wireless` com licenças,
SHA e commit fixados. **Esses fontes não são drivers executáveis do LunaOS:** scan,
associação e configuração automática de Wi-Fi ainda não estão implementados.
O assistente permite seguir offline e não simula a conexão.

O instalador usa somente ATA/IDE legado de 512 bytes e LBA28; AHCI/NVMe/USB e
UEFI continuam pendentes. Instalação física geral ainda não é suportada. Consulte
[setup.md](docs/setup.md) para limites, formato do disco, credenciais e comandos de
QEMU com disco virtual descartável.


### Resolução nativa

Corrigida a troca de resolução: o compositor, wallpaper, caches e cursor
usam as dimensões reais do modo; o desktop inteiro não é mais ampliado de 1024×768.
Em escala 100%, fontes e ícones mantêm seu tamanho em pixels, com mais área de
trabalho nas resoluções maiores. Barra superior, dock, busca e painel rápido são
reposicionados; janelas maximizadas seguem a nova área útil. O setup/login continua
centralizado. `tests/native_resolution.py` confere medidas de texto e controles
nas capturas, limites do mouse e persistência até 1920×1080, com 64 MiB de RAM.


### ISO e mídia óptica

`make` gera `.img` e `.iso`, com SHA-256 para ambos. A ISO inicia pelo bootloader
próprio com El Torito e continua lendo a origem pelo driver ATAPI nativo. A mídia
óptica é somente leitura, e o instalador pode copiar para um disco IDE separado.
Veja [iso.md](docs/iso.md) para comandos, testes e limites BIOS/ATAPI.

### Base compartilhada de Wi-Fi e Intel AX211

A API preserva BSSID, canal e segurança de até 32 redes e a configuração exibe
2,4 ou 5 GHz. O inventário Intel reconhece o AX211 desta máquina
(`8086:51f0`, subsistema `8086:4090`), mas **scan e conexão no AX211 ainda
dependem de um backend Intel nativo**. A conexão Wi-Fi continua indisponível.
Há agora parser e firmware Intel licenciado, preparação PCI/MMIO (`wifidiag intel`),
handshake WPA2-PSK/CCMP compartilhado, porta de dados e integração DHCP/IPv4.
Essas camadas passam por testes com rádio simulado; o boot do firmware e o rádio
Intel ainda não estão implementados. Arquitetura, limites e etapas restantes:
[Intel AX211](docs/intel-ax211-wifi.md).

### Memória física e heap sob demanda

O kernel agora aloca páginas nas regiões utilizáveis do mapa E820/UEFI, sem teto
de 64 MiB e sem os antigos pools fixos de 2 MiB para DMA e 8 MiB para o runtime.
Heap, JavaScript e drivers passam a crescer conforme a RAM física disponível.
`free` mostra memória descoberta, endereçável, reservada, alocada e livre;
`memtest 96` verifica 96 MiB de heap + DMA e a devolução das páginas.

O kernel permanece i386 sem PAE: RAM remapeada acima de 4 GiB é informada, mas
ainda não pode ser alocada. Isso inclui parte dos 4 GiB instalados em máquinas
que remapeiam RAM por causa de MMIO. Detalhes e testes: [memória física](docs/physical-memory.md).

### Consumo de RAM por processo

O Task Manager mostra **RAM** por PID (páginas de heap + pilhas + tabelas de
páginas), com arenas físicas exclusivas por processo e liberação no encerramento.
O kernel mantém sua própria conta de drivers e buffers compartilhados, sem contar
novamente o heap dos apps. `ps` detalha RAM, heap e pilha. Não há quota fixa por
PID; os backends continuam sendo serviços do kernel, sem isolamento completo de
heaps de usuário. [Implementação e testes](docs/process-memory.md).

O relógio da barra superior abre um calendário mensal. Em **Configurações > Data e hora**,
é possível ajustar data/hora offline, escolher o fuso UTC e sincronizar por NTP
com `time.cloudflare.com` (servidor configurável). Veja [calendário e horário](docs/calendar.md)
para comandos, persistência, limites e testes.
