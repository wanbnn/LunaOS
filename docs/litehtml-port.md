# litehtml no Luna Browser

A build padrão usa Lexbor v3.0.0 para interpretar o HTML e litehtml v0.10 para
aplicar CSS, calcular o layout e desenhar a página. Inicialize os submódulos com
`git submodule update --init --recursive`. A build requer CMake e G++ com as
bibliotecas estáticas de 32 bits. Use `make LUNA_LITEHTML=0` para selecionar o
renderizador legado com Lexbor.

O adaptador em `ui/web_lexbor.c` copia a árvore interpretada para `WebNode`,
usada pelo QuickJS. `ui/litehtml_bridge.cpp` cria os elementos de layout a partir
dessa árvore. O Gumbo incluído no litehtml inicializa apenas um documento vazio,
que o adaptador preenche com os elementos do Luna. Mudanças feitas por scripts
reconstroem a representação de layout. Cliques em links usam as caixas do
litehtml e são encaminhados ao nó correspondente no DOM do Luna.

O litehtml aplica o CSS embutido e as folhas vinculadas que o navegador baixa.
O desenho de imagens usa o cache de imagens decodificadas do Luna. A integração
ainda depende dos limites de recursos do navegador e de sua implementação de
JavaScript e formulários. Gradientes e carregamento de novos recursos por
`@import` ainda não estão implementados.

O primeiro layout usa as folhas externas já carregadas. Folhas grandes são
processadas em grupos de regras completas. O QuickJS executa scripts
clássicos embutidos e externos em ordem de documento. Os temporizadores
rodam na tarefa do navegador e as alterações de DOM pedem novo layout.

O litehtml pode usar a memória física endereçável que o kernel tiver
disponível. Ao expandir uma arena, o alocador preserva 1 MiB de páginas
livres para o sistema. O prazo de CPU para criar e recalcular o layout
cresce com o número de nós. Quando o renderizador falha, o Luna mantém
o documento aberto com o renderizador legado.

`make test-litehtml` usa os mesmos patches que a imagem do sistema.
Os patches cobrem a largura e o avanço de elementos flutuantes e as
colunas de Grid usadas nas páginas de patrocínio do Python.org.

O kernel liga a biblioteca litehtml, seu Gumbo e o runtime estático do G++.
Os submódulos preservam as licenças do litehtml (BSD-3-Clause) e do Gumbo
(Apache-2.0). As imagens incluem esses avisos e a exceção de runtime do GCC.
