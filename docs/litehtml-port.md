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

O navegador mostra o documento enquanto baixa os recursos externos. Folhas de
estilo grandes são processadas em grupos de regras completas, e o layout de
elementos flutuantes tem um limite de tentativas por caixa. Se o litehtml atingir
o limite de recursos, o Luna mantém o documento aberto com o renderizador legado.

O kernel liga a biblioteca litehtml, seu Gumbo e o runtime estático do G++.
Os submódulos preservam as licenças do litehtml (BSD-3-Clause) e do Gumbo
(Apache-2.0). As imagens incluem esses avisos e a exceção de runtime do GCC.
