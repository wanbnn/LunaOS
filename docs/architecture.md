# Arquitetura do LunaOS 0.3

## Boot e memória

O setor de boot próprio usa EDD para carregar 32 KiB do segundo estágio em `0x8000`.
O stage 2 configura VBE, consulta E820/EDID e lê o kernel, começando no setor 65,
em blocos de até 32 KiB para um bounce buffer em `0x10000`. Entre leituras, entra em
protected mode, copia para RAM a partir de `0x100000`, volta ao BIOS e lê o próximo
bloco. No final entra no kernel com pilha em `0x3f00000`.

O kernel é ligado em 1 MiB; BSS começa alinhado após código/dados. O linker mantém
BSS abaixo de `0x3d00000`. O gerador rejeita um kernel que alcance os slots LunaFS.
A versão atual exige 64 MiB. Reservas estáticas incluem buffers de tela, superfícies
de até sete janelas, pools de pilhas, tabelas de páginas, rede e heap limitado do JS.
Não há alocação virtual dinâmica de processos gerais.

## Kernel e rings

A GDT tem código/dados para rings 0/1/2/3 e TSS. O TSS define a pilha de entrada no
kernel para cada cliente. O contexto comum de interrupção preserva registradores e
seletores, e retorna com IRET. `int 0x80` é a ABI controlada de clientes;
`int 0x81` é yield interno, inacessível diretamente a ring 3.

Rings não zero usam seletores com base `0x40000000` e limite `0x13fff`. O código
cliente ocupa uma página read-only; quatro páginas privadas em `0x40010000` formam
a pilha. Cada tarefa tem CR3 distinto. Mapas do kernel são supervisor-only.

Na paginação x86, rings 1/2 também são supervisor; não são a mesma barreira de páginas
do ring 3. Por isso continuam reservados a serviços confiáveis. As tarefas de usuário
comum são clientes ring 3. A referência é o [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

O kernel/compositor é protegido contra kill. Clientes têm capacidades restritas e
mailboxes próprias. Os backends de terminal, arquivos e GUI ainda ficam em ring 0;
os clientes não são executáveis gerais com todo seu código em modo usuário.

O PIT interrompe a 1 kHz. `frame_clock` conta milissegundos; `ticks` preserva o clock
lógico de 100 Hz para timeouts existentes. O timer permite preempção de rings não
zero. Código de kernel é cooperativo: rede, eventos e sleep cedem a CPU. JS tem um
orçamento curto de execução, mas uma rotina longa de kernel ainda pode atrasar frames.

## Entrada, janelas e renderização

PS/2 mantém filas de teclas e transições de botões. Modificadores são capturados no
momento do evento. Super abre o launcher; Alt+Tab usa uma ordem de troca estável enquanto
Alt está pressionado; Alt+Shift+Tab reverte; Alt+F4 fecha o foco. F1–F6 não abrem apps.
Movimentos repetidos com o mesmo estado de botão são agrupados, preservando as bordas
de pressionar/soltar. Deltas PS/2 usam o sinal de nove bits e escala linear configurável.

Janelas mantêm geometria, foco, ordem, restauração, minimização e PID. O compositor
pinta cada cliente em uma superfície RGB cacheada. Arrastar recompõe superfícies;
ao soltar, invalida o vidro para recalcular a amostragem do fundo na nova posição.
Recortes respeitam as dimensões da janela; as curvas usam cobertura de 16 subpixels.

O scanout preferido é Bochs/QEMU stdvga de 32 bits; VBE é fallback. Cópias em bloco
substituem a conversão completa por pixel no caminho nativo. O cursor é um overlay
pequeno restaurado a partir da cena, com hotspot alinhado à ponta do Lucide. Ele não
invalida todos os aplicativos a cada movimento.

O pacing usa fase fracionária (`milliseconds * 60`, incremento de 1000) para não
introduzir um teto de 58,8 fps por arredondamento constante de 17 ms. Idle não força
60 repaint completos por segundo. `bench` mede throughput, separado da cadência
interativa e da frequência física. Timings do monitor não são programados pelo driver;
modos superiores a 60 não são anunciados como suportados.

## Configurações e dados

Settings é um cliente distinto do painel rápido da barra superior. Preferências
alteram PS/2, mistura AC97, LUT de saída, estilos e papel de parede. Fundos próprios
usam PPM P3 limitado e interpolação bilinear. Preferências são texto em LunaFS e a
persistência depende do botão de salvar/sync.

LunaFS segue com 64 entradas de texto de até 8191 bytes. O disco raw tem 32.768 setores:
boot em 0, stage 2 nos setores 1–64, kernel após 65 e snapshots em 8192/12288.
Snapshots alternados têm geração, tamanho e FNV-1a; o cabeçalho é publicado por último.
Não há FAT/ext, permissões multiusuário, binários, instalador ou estabilidade de formato.

## TLS, JS e motor web

A pilha RTL8139 usa DMA físico e polling com ring RX de 64 KiB. TCP tem um fluxo
síncrono, janela, ACK e retransmissão básica de escritas. O backend cede a CPU durante
esperas. Há DNS IPv4, arquivo hosts, HTTP/1.0 e chunked, com documentos até 128 KiB.

BearSSL fornece TLS 1.2 com ECDHE/AEAD. A aplicação injeta RDRAND e data RTC, valida
cadeia e hostname, e não permite ignorar certificados. A confiança padrão é um snapshot
público de 120 raízes; uma CA adicional só é lida de um arquivo explicitamente instalado.
Respostas truncadas são rejeitadas; framing HTTP completo autenticado pode encerrar a
mensagem quando o peer omite close_notify, mas outros erros TLS não são ignorados.

QuickJS fornece o comando `js`, com heap/stack limitados e timeout. Apenas objetos
ISO-C/math de musl são usados; allocation, diagnostics e relógio são hooks do LunaOS.
Não existe ABI Linux. O código embutido de cliente é simples e não usa FPU; o serviço
JS usa x87 inicializado. Isso não fornece isolamento completo de estado FPU para
programas externos, que ainda não podem ser carregados.

**Não existe um motor DOM/CSS integrado.** O leitor HTTPS e o runtime JS isolado não
cumprem a exigência de páginas modernas. A avaliação e os pré-requisitos do porte real
estão em [browser-engine.md](browser-engine.md).

## Validação e trabalho pendente

Testes inicializam cópias da imagem: boot, arquivos/snapshot, rede, áudio, rings e
privilégios, lifecycle de janelas, novos atalhos, autocomplete, persistência de settings,
TLS verificado, recusa de CA/hostname e scripts com timeout. Relatórios registram
SHA-256; o fixture de DOM/CSS moderno permanece explicitamente não executado no guest.

Ainda faltam loader/ABI para apps completos, IPC, threads, memory mappings e runtime
C++ de processo, porte do engine web e dependências, UEFI, USB, NVMe/AHCI, Wi-Fi e
Bluetooth. O monitor de tarefas mostra reservas de pilhas e CPU medida, não RSS
inventado nem uma compatibilidade de aplicações que ainda não existe.


## Serviços de drivers v1

O kernel reserva um pool de DMA de 2 MiB validado contra E820 e uma arena MMIO
supervisor em 0x3c000000 (16 MiB) com PCD/PWT. As tabelas dessa arena são partilhadas
pelos diretórios de página e mudanças invalidam TLB; o switch de CR3 invalida as
entradas dos outros contextos. Mapeamentos/claims/IRQs têm handles e lifetime.
Acesso CF8/CFC é serializado com IRQ-save, inclusive para os drivers anteriores.

As entradas IDT 33–47 despacham INTx compartilhado e retornam ao mesmo contexto;
o timer mantém sua lógica de scheduler. Work/timers rodam cooperativamente no
loop de kernel com orçamento de 16 callbacks, nunca no ISR do PIT. O repositório
contém um teste de serviço com EDU, não um porte mac80211 concluído. Ver contrato,
limites e formato binário de firmware em [driver-services.md](driver-services.md).


## Primeiro boot e instalação

`ui/setup.c` intercepta teclado, ponteiro e desenho enquanto o assistente ou login
está ativo. O desktop live exige uma ação explícita e não escreve um marcador de
instalação. `/etc/setup-complete` só é salvo no destino após confirmação do disco.
O backend ATA enumera até quatro dispositivos; o boot drive e ID MBR vêm do stage2.
Cópia em blocos tem comparação de leitura; o MBR do destino é confirmado no fim.
`kernel/account.c` calcula PBKDF2-HMAC-SHA256 incremental e confere senha antes de
liberar a sessão. O modelo segue sendo usuário único, sem ACL/encriptação de disco.
A imagem de 32 MiB carrega os fontes Linux Wi-Fi como arquivo comprimido de dados,
sem tratar os fontes como drivers executáveis. Ver [setup.md](setup.md).


## Canvas nativo e caches limitados

`screen_width`/`screen_height` correspondem aos pixels do framebuffer. Scene e
wallpaper reservam até 1920×1080 cada, mas cópia, pitch e clipping usam o tamanho
ativo. O wallpaper de prévia é calculado analiticamente, sem um terceiro buffer
grande. Não há reamostragem da cena inteira no scanout.

As sete superfícies pequenas mantêm stride 1024 e altura 606. Uma superfície
grande compartilhada atende janelas maiores/maximizadas; sua posse invalida o
cache ao alternar de cliente. O stride é explícito no blit. Janelas inteiramente
cobertas são omitidas. O BSS continua abaixo do limite de 61 MiB reservado pelo
linker e a versão permanece inicializável com 64 MiB. Tamanho de fonte/ícone em
pixels não depende da resolução: escala de interface atual é 100%.
