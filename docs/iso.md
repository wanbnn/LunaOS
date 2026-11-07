# ISO inicializável do LunaOS

`make` gera os dois formatos:

- `dist/LunaOS.img`: disco raw de 32 MiB, boot BIOS por EDD, gravável.
- `dist/LunaOS.iso`: ISO9660 de 32,125 MiB, El Torito, bootloader Luna próprio,
  mídia somente leitura com suporte ATAPI nativo no guest.

O orçamento de 120 MB é respeitado por ambos. Não há GRUB ou kernel Linux.
A ISO usa o mesmo kernel, fontes e arquivos da imagem raw. Os fontes Wi-Fi Linux
continuam sendo material de desenvolvimento, não drivers de rádio executáveis.

## Executar

```sh
make run-iso
```

Ou com QEMU instalado no host:

```sh
qemu-system-i386 -m 64M -vga std -cdrom dist/LunaOS.iso -boot d
```

Para instalar em um disco virtual novo:

```sh
python3 tools/create-target.py build/luna-target.img 128
LUNA_IMAGE=dist/LunaOS.iso tools/run-qemu.sh -drive file=build/luna-target.img,format=raw,if=ide,index=0
```

IDE 0 será o destino neste comando. O CD costuma aparecer como IDE 2, protegido.
Conferir modelo/capacidade e a confirmação na interface. Após instalar, iniciar
sem o CD:

```sh
LUNA_IMAGE=build/luna-target.img tools/run-qemu.sh
```

Alterações de uma sessão live do CD ficam na RAM; `sync` não grava na mídia óptica.
As configurações da instalação são gravadas no destino escolhido, não no CD.

## Formato e boot

`tools/mkiso.py` é um gerador ISO9660/El Torito em Python padrão; não requer
xorriso/mkisofs. Há volume primário, boot descriptor, terminador, tabelas de
caminho LE/BE, diretório raiz e catálogo com checksum. O catálogo aponta para uma
imagem de disco adaptada no setor óptico 64, também visível como `LUNAOS.IMG`.
O SHA da imagem base e o SHA da variante embarcada ficam em `iso-manifest.json`.
O arquivo interno é adaptado ao CD; para uso raw, usar o `.img` distribuído à parte.

O BIOS fornece emulação de disco de 512 bytes via El Torito. O loader detecta o
marcador da variante CD e usa CHS legado com a geometria anunciada no catálogo;
isso também funciona em SeaBIOS, que não oferece EDD para o disco emulado. Boot
raw continua usando EDD. O segundo estágio entrega a localização e identidade
da imagem óptica ao kernel.

Depois do protected mode, o kernel identifica ATAPI no IDE, envia PACKET/READ(12)
e converte os blocos ópticos de 2048 bytes para a visão raw de 512 bytes. A origem
é validada pelo identificador do boot. Se não for encontrada, não substitui a
origem silenciosamente por um disco gravável. O driver recusa operações de escrita
no CD. Ao instalar, os marcadores CD são removidos do MBR do destino antes do commit.

Limites: boot **BIOS**, não UEFI; ISO não híbrida USB; ATAPI legado, sem AHCI/NVMe
ou USB mass storage. Não fornece um browser geral de arquivos ISO9660 dentro do
LunaOS. A ISO não adiciona suporte Wi-Fi ou Bluetooth.

## Verificação

```sh
make check
LUNA_TEST_DIR=build/test-iso PYTHONPATH=.tools/python python3 tests/iso.py
make test-iso-install
```

O teste de ISO inicia o QEMU somente com CD, valida setup, acesso aos arquivos e
rejeição de escrita. O teste de instalação usa arquivos de disco descartáveis,
confere a origem e outro disco intactos, reinicia o destino sem o CD e verifica
login e configurações. Os SHA-256 dos dois formatos ficam em `dist/SHA256SUMS`.
