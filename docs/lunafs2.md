# LunaFS2 — arquivos variáveis, conteúdo binário e persistência

A restrição de **8191 bytes por arquivo** foi removida. O FS agora guarda
um ponteiro e comprimento por entrada, com armazenamento na heap sob demanda.
A função `fs_write_bytes(path, data, length, append)` aceita conteúdo binário
(incluindo bytes `0x00`). `fs_write` permanece como atalho para texto
terminado por NUL. A leitura via `fs_find` expõe `File.data` e `File.size`;
leitores binários não devem usar `strlen` para determinar o tamanho.

## Layout no disco e durabilidade

- Duas regiões fixas de snapshot iniciam nos setores 8192 e 12288,
  com 4096 setores cada (2 MiB/slot). O primeiro setor é um header `LUNAFS2`;
  o payload contém um conjunto de entradas e dados concatenados.
- Apenas bytes ocupados são gravados. O hash cobre os bytes serializados.
  O header só é confirmado depois de completar a gravação dos dados;
  uma falha interrompida preserva o snapshot válido anterior.
- A inicialização lê e valida tanto `LUNAFS1` (legado de 8 KiB) quanto
  `LUNAFS2`. O próximo `sync` salva na versão nova.
- Arquivos são alocados sob demanda; não há buffer fixo de 8 KiB por entrada.
  O volume continua limitado a **64 entradas**, com payload máximo de
  **2.096.640 bytes** por snapshot menos metadados de diretório.
- **Isso não transforma o espaço do pendrive/SSD em disco de dados LunaOS.**
  O kernel atual só escreve em IDE/ATA. Pendrives UEFI são lidos previamente
  pelo firmware e entregues como imagem RAM read-only após ExitBootServices.
- CPython, bibliotecas e executáveis grandes ainda precisam de um filesystem
  de capacidade redimensionável em um controlador de armazenamento nativo.
  Este commit remove o limite por arquivo, **não o limite do volume**.

## Riscos e compatibilidade

O formato v1 migra na próxima gravação, mas snapshots em v2 **não podem ser
lidos por kernels antigos**. Faça backup antes de voltar para uma versão
anterior. Binários maiores do que o total disponível do volume devem ser
recusados sem truncamento. O instalador preserva o conteúdo anterior usando
backups profundos que não duplicam a posse dos buffers.

## Testes

```sh
make test-lunafs2
make check
```

O teste host verifica binários acima de 8 KiB, conteúdo NUL, load/save,
retrocompatibilidade com v1, rollback de escrita interrompida e backup profundo.
A instalação real e o boot pelo hardware físico continuam exigindo testes
independentes.
