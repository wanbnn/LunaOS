# Primeiro boot e instalação LunaOS

A imagem de distribuição abre o assistente nativo. A interface usa Inter Tight, Noto Sans ou Montserrat, escolhidas em Configurações,
ícones Lucide e os mesmos recursos gráficos do desktop. Não é um instalador Linux.

## Fluxo implementado

1. Boas-vindas com saudações alternadas em português, inglês, espanhol, francês,
   alemão e italiano. “Experimentar LunaOS”/Esc abre o desktop live sem marcar
   instalação concluída nem escrever automaticamente no disco.
2. Idioma do sistema/assistente: inglês ou português; teclado US ou ABNT2 básico,
   aplicado imediatamente, com campo para teste. A tradução do desktop é parcial.
3. Identificação PCI do adaptador Wi-Fi e indicação real do estado do driver.
4. Rede: mostra Ethernet RTL8139 e uma tela de Wi-Fi com botões Buscar redes,
   seleção de SSIDs, campo de senha e Conectar. A UI usa exclusivamente dados
   retornados pela API `kernel/wifi.h`; **nenhum SSID é inventado**.
   O RTL8822BE possui scanner passivo nativo e um caminho **experimental**
   de transmissão de quadros, autenticação/associação e WPA2-PSK/CCMP, ligado
   ao DHCP da pilha de rede. O botão Conectar está disponível apenas quando
   o rádio inicializa e a rede oferece um modo compatível. **Ainda não foi
   validada uma conexão real em hardware**: o rádio pode não transmitir,
   associar ou obter IP nesta versão. O assistente também permite continuar
   offline. O diagnóstico detalhado está em `docs/rtw8822be-native-port.md`.
5. País/região e preferência de zona de horário, salvos em `/etc/region`.
   Não há banco IANA/DST integrado; o relógio atual continua UTC.
6. Usuário padrão, senha e confirmação. Nome: inicia com a-z e contém até 24
   caracteres a-z/0-9/_/-. Senha: 8–64 bytes ASCII.
7. Escolha explícita do disco inteiro: modelo, índice do dispositivo e capacidade. A mídia
   de origem, suas cópias com o mesmo identificador e discos pequenos/não suportados
   ficam indisponíveis. Nenhum destino vem selecionado.
8. Revisão do destino e digitação de `apagar N`/`erase N`, onde N é o índice
   escolhido. Só o botão confirmado inicia a operação destrutiva.
9. Hash da senha, formatação rápida, cópia com leitura de verificação,
   persistência/verificação da configuração, commit do boot e reinício automático.
10. Ao iniciar pelo destino, uma tela de entrada exige a senha criada. Após entrar,
    `$HOME`, `pwd`, Files e `whoami` usam o usuário configurado.

Pode-se reabrir o assistente no desktop live com `setup` no lunaSH.
Em uma instalação concluída, esse comando recusa reinstalar pela sessão ativa.

## Discos e durabilidade

O backend oferece quatro slots ATA/IDE legados e oito slots nativos. Além de
AHCI e NVMe, há leitura, gravação e flush por USB EHCI/BOT, SDHCI/eMMC e
MegaRAID MFI SAS1078/2108. Todos usam setores lógicos de 512 bytes. O setup
oferece como destino apenas dispositivos inicializados, graváveis e dentro dos
limites da API. A lista mostra apenas dispositivos presentes e abre na página
com o primeiro destino disponível, sem selecioná-lo automaticamente. Use
esquerda/direita para navegar quando houver mais de quatro dispositivos.
Se nenhum destino estiver disponível, execute `storage` no Terminal do modo
live para obter o diagnóstico PCI/ACPI e dos drivers; slots IDE vazios não
indicam ausência de discos SATA/NVMe/eMMC.

USB exige dispositivo high-speed em porta raiz EHCI e protocolo BOT/SCSI;
xHCI, hubs, UAS e HC legados continuam sem driver. SDHCI usa PIO 1-bit para
SDHC/SDXC ou eMMC com endereçamento por setor (somente área de usuário).
MegaRAID acessa volumes lógicos já configurados; não cria nem modifica arrays.
Intel RST/VMD e outros protocolos RAID continuam não suportados. A descoberta
PCI de um modelo fora desses limites não o torna destino de instalação.
Consulte [armazenamento](storage.md) para testes e limitações.

O loader entrega ao kernel o drive BIOS e o identificador MBR da mídia de boot.
A descoberta ATA procura esse identificador para selecionar a origem; o instalador
nunca oferece o disco de origem como destino. O volume de instalação precisa estar
acessível ao backend IDE. O layout da imagem é verificado antes de começar.

Após confirmação, o instalador invalida o boot do destino, limpa metadados GPT do
final do disco e copia a imagem. O começo da imagem substitui também o GPT primário.
Cada bloco copiado é lido de volta e comparado. O formato é MBR, com uma entrada
LunaOS 0xda iniciando no setor 8192; o loader próprio lê seus estágios antes disso.
O LunaFS2 mantém até 64 entradas e grava arquivos binários de tamanho variável.
O limite de 8 KiB por arquivo foi removido, mas o armazenamento persistente ainda
cabe em duas regiões reservadas de ~2 MiB para snapshots; a capacidade disponível
no disco/pendrive não se torna automaticamente espaço para arquivos. O formato v1
continua legível para migração.

A configuração e o hash vão para snapshots LunaFS no **destino**, com flush e
verificação. O setor de boot assinado e o novo identificador de disco só são
confirmados no fim. Em falha, não tenta outro disco e não declara sucesso.
Formatação rápida **não é apagamento seguro** dos dados anteriores. Uma instalação
interrompida pode deixar o destino sem boot; não há rollback dos dados apagados.
A origem e os demais discos não são modificados pelo assistente.

O reinício é solicitado automaticamente após a tela final. Retirar a mídia de
instalação ou mudar a prioridade do BIOS é necessário para iniciar pelo destino;
o instalador não muda silenciosamente a configuração do firmware do computador.

## Usuário e senha

`kernel/account.c` usa HMAC-SHA256 BearSSL para PBKDF2 com 210.000 iterações e salt
aleatório de 16 bytes via RDRAND. O trabalho é incremental para não bloquear toda
a interface. Sem entropia, a instalação recusa criar credenciais. Não existe senha
padrão nem fallback de texto simples. Buffers de senha/KDF são limpos após uso;
a confirmação não é salva nem enviada pelo serial. A verificação do hash compara
todos os bytes sem terminar no primeiro byte diferente.

`/etc/luna-user` contém algoritmo, nome, iterações, salt e hash. Essa entrega
implementa uma barreira de entrada para uma sessão local de usuário único.
Não implementa criptografia de disco, secure boot, permissões multiusuário/ACL
ou recuperação de senha. Dados de conta inválidos em uma instalação marcada
como concluída abrem erro de recuperação, sem liberar o desktop automaticamente.
Super, atalhos de apps e entrada serial não contornam a tela de entrada.

## Fontes Wi-Fi na imagem

`assets/driver-sources/linux-wireless.tar.gz` contém o snapshot completo de
`drivers/net/wireless`, mais COPYING e LICENSES. Commit, tamanho e SHA-256 estão em
`assets/driver-sources/manifest.json`. O pacote comprimido ocupa cerca de 11 MB;
a imagem total tem **32 MiB**, abaixo do limite de 120 MB.

O arquivo é armazenado a partir do setor 32769, após um header `LUNASRC1` no setor
32768 com comprimento, SHA-256 e commit. A cópia da imagem também leva esse pacote
para o destino. Ele não é um driver binário nem traz um compilador/ABI Linux para
o guest: **não resolve automaticamente o rádio durante a instalação**. O assistente
mostra a ausência de suporte e não inventa redes ou uma conexão bem sucedida.
O próximo passo continua sendo adaptar os drivers às APIs nativas e portar MAC,
firmware específico, autenticação e integração IP. Ver `wireless-port.md`.

Para atualizar o pacote, usar um checkout sparse fixado do Linux e executar
`python3 tools/vendor_wireless.py CAMINHO_DO_CHECKOUT`. A build verifica o SHA antes
de incluí-lo; não faz download automático de branches variáveis durante `make`.
Os fontes não são compilados como parte do kernel LunaOS.

## Teste seguro no QEMU

Criar **um arquivo novo** de disco virtual de destino; não apontar um dispositivo
físico do host. Exemplo:

```sh
python3 tools/create-target.py build/luna-target.img 128
tools/run-qemu.sh -drive file=build/luna-target.img,format=raw,if=ide,index=1
```

No assistente, IDE 0 é a mídia e fica protegida; selecionar IDE 1 após conferir
a capacidade. Usar uma imagem nova ou descartável de destino. Não usar o `-snapshot`
global para uma instalação persistente: QEMU descartaria as alterações do destino.
Após instalar, iniciar apenas o destino, por exemplo com os mesmos dispositivos:

```sh
qemu-system-i386 -m 64M -vga std -drive file=build/luna-target.img,format=raw,if=ide -netdev user,id=n0 -device rtl8139,netdev=n0
```

Se QEMU/SeaBIOS são os binários locais de `.tools`, aproveitar o launcher com
`LUNA_IMAGE=build/luna-target.img tools/run-qemu.sh`.

`tests/setup.py` usa arquivos descartáveis e verifica: live sem escrita, origem
protegida, token incorreto recusado, outro disco intacto, cópia, reinício, boot sem
mídia original, senha incorreta recusada, hash conferido independentemente com
`hashlib.pbkdf2_hmac`, home e preferências persistidos. `dist/setup-report.json`
registra o SHA da imagem testada. Não é teste de Wi-Fi real ou de hardware físico.
