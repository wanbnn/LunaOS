# LunaFS2 e arquivos em disco

O diretório cresce na heap conforme a demanda. Os limites antigos de 64 entradas
e 8191 bytes por arquivo foram removidos. Arquivos grandes permanecem no disco,
inclusive depois de reiniciar o sistema.

## Downloads em streaming

O `wget URL ARQUIVO` recebe e grava blocos de tamanho constante. O tamanho do
arquivo pode superar a RAM disponível. O caminho HTTP aceita Content-Length,
transferência chunked com trailers e respostas delimitadas pelo fechamento da
conexão. A validação TLS continua ativa nos downloads HTTPS.

A preparação do snapshot ocorre antes de abrir a conexão, incluindo a cópia
dos arquivos existentes para a área inativa do volume. O destino
anterior fica disponível durante o download. O novo tamanho, o checksum e o
cabeçalho são confirmados após receber uma resposta completa e descarregar os
dados no disco. Uma transferência incompleta, falha de escrita ou cancelamento
preserva o snapshot anterior.

O download concluído já está persistido. A confirmação também inclui os demais
arquivos e alterações presentes no snapshot. Alterações pequenas feitas pelos
editores continuam usando cache em RAM até uma gravação de snapshot, como `sync`.

## Leitura e escrita

`File.data` é um cache opcional. Leitores nativos usam
`fs_read_bytes(file, offset, buffer, length)` para acesso em blocos. Arquivos de
até 64 KiB podem ser carregados em cache. Esse valor orienta o uso de memória.
`fs_materialize` permite solicitar um buffer completo quando o consumidor dispõe
de memória. `fs_read_text` recusa buffers insuficientes.

`fs_write_bytes` aceita conteúdo binário, incluindo bytes NUL. Escritas grandes
e acréscimos usam a transação em disco. `fs_copy` copia arquivos em blocos e
preserva seu conteúdo binário. O `wc` percorre o arquivo em blocos.

O LunaSDK JavaScript aceita `luna.fs.read(path, offset, length)` para leituras
parciais. Omitir offset e length mantém a leitura integral existente, sujeita à
memória disponível ao aplicativo. A prévia do gerenciador de arquivos lê um
prefixo limitado. O navegador e os editores mantêm seus próprios orçamentos de
conteúdo visualizável.

Backups de estado copiam os caches e mantêm referências protegidas aos snapshots
que contêm arquivos grandes. A gravação recusa sobrescrever um snapshot ainda
referenciado por um backup ativo.

## Volume e compatibilidade

Em discos LunaOS identificados pelo MBR e pelo cabeçalho da imagem, a gravação
pode criar uma partição de dados no maior intervalo livre após a imagem do
sistema. As partições existentes são preservadas. A partição usa tipo MBR `0xda`
e identificação `LFS` nos campos CHS sem uso.

A área é dividida em dois snapshots. Um arquivo de 1 GiB exige espaço para os
dois snapshots, os demais arquivos e a imagem do sistema. Uma imagem raw de
exatamente 66 MiB conserva a área original disponível. Para ampliar uma cópia
em QEMU, use por exemplo `truncate -s 3G copia.img` antes de iniciá-la.

Os snapshots antigos nos setores 8192 e 12288 continuam legíveis. A migração
gravada na partição expandida preserva os snapshots originais. O instalador cria
a área no destino e a atualização do sistema preserva sua tabela de partições.
Os controladores nativos compartilham a mesma interface de blocos.

O formato continua usando comprimentos de snapshot de 32 bits e caminhos de
128 bytes. O total serializado de cada snapshot precisa caber nesse formato e
no espaço disponível. A área de dados exige uma mídia gravável e um controlador
nativo compatível com setores lógicos de 512 bytes.

## Verificação

- `make test-lunafs2` cobre diretório dinâmico, migração, conteúdo binário, cópia,
  acréscimo, isolamento de partições e recuperação de escrita interrompida.
- `make test-fs-streaming` passa uma resposta HTTP de 1 GiB mais 123 bytes pelo
  decoder e pelo LunaFS reais, com o processo limitado a 128 MiB de espaço de
  endereçamento e a heap do filesystem limitada a 2 MiB. Compara todo o conteúdo
  e verifica recarga, interrupção e proteção de snapshots referenciados.
- `tests/https.py` verifica streaming HTTPS, chunked, trailers incompletos,
  certificados, preservação do destino e persistência em QEMU.
- `tests/download_streaming.py` baixa 96 MiB mais um byte em QEMU com 64 MiB de
  RAM e controlador AHCI nativo. Compara o SHA-256 completo e verifica recarga e
  leitura parcial pelo SDK.

Para repetir o teste nativo com um download de 1 GiB mais 123 bytes, execute
`LUNA_DOWNLOAD_BYTES=1073741947 LUNA_TEST_DIR=build/test-download-gib python3 tests/download_streaming.py`.
A imagem descartável cresce conforme o tamanho solicitado.
