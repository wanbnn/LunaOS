# LunaFS2 e capacidade de armazenamento

A tabela de arquivos e os dados crescem na heap conforme a demanda. Os antigos
limites de 64 entradas e 8191 bytes por arquivo foram removidos. A API
`fs_write_bytes` preserva conteúdo binário, incluindo bytes NUL.

## Volume persistente

Em discos LunaOS identificados pelo MBR e pelo cabeçalho da imagem, o primeiro
`sync` pode criar uma partição de dados no maior intervalo livre após a imagem
do sistema. Partições existentes são preservadas. A partição usa o tipo MBR
`0xda` e uma identificação `LFS` nos campos CHS sem uso.

A área disponível é dividida em dois snapshots. O tamanho acompanha o espaço
livre encontrado no dispositivo. Uma imagem raw de exatamente 66 MiB continua
com a área original disponível. Para testar a expansão em QEMU, aumente uma
cópia da imagem antes de iniciá-la, por exemplo com `truncate -s 128M copia.img`.

Os snapshots antigos nos setores 8192 e 12288 continuam legíveis. A migração
preserva esses snapshots e grava a nova versão na partição expandida. Cada
snapshot contém o diretório e os dados ocupados, protegidos por checksum. A
confirmação ocorre após gravar e descarregar os dados. Uma gravação interrompida
permite restaurar o snapshot válido anterior.

O instalador cria a área no disco de destino. A atualização do sistema preserva
a tabela de partições do disco instalado.

## Recursos e formato

A capacidade depende do disco e da RAM disponíveis. Arquivos permanecem em RAM
enquanto o sistema funciona e a gravação substitui seu buffer de forma atômica.
Os snapshots continuam usando comprimentos de 32 bits e caminhos de 128 bytes.
O dispositivo precisa ser acessível por um controlador nativo com setores de
512 bytes. Mídias somente leitura permitem trabalho em RAM e exigem um destino
gravável para persistência.

O `wget` usa buffers expansíveis, sem os antigos tetos de 8 KiB no comando ou
512 KiB no transporte de download. Respostas HTTP incompletas são rejeitadas e
preservam o arquivo de destino existente.

## Verificação

`make test-lunafs2` verifica migração, conteúdo binário, mais de 64 entradas,
arquivo de 3 MiB, isolamento de partições e recuperação de escrita interrompida.
`tests/https.py` exercita o download chunked de 3 MiB no sistema em QEMU.
