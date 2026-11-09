# Teste de uso do Luna Browser

## Escopo

Teste manual da build local em QEMU com 64 MiB de RAM, usando mouse e teclado do
LunaOS. O teste percorreu o Python.org, a documentação do Python e o Google. As
capturas da primeira sessão registram a sessão real e estão em
[build/browser-usability](../build/browser-usability/); as do reteste estão em
[dist](../dist/).

## O que funcionou

- **Carregamento do Python.org.** A página inicial abriu por HTTPS com CSS e
  logotipo. O travamento completo do sistema observado antes da correção não
  voltou a ocorrer durante esta sessão. [Captura](../build/browser-usability/01-python-inicio.png)
- **Navegação por links.** O link **Docs** levou de `www.python.org` a
  `docs.python.org/3/`. O link **aqui** na página intermediária do Google também
  abriu o destino indicado. [Docs](../build/browser-usability/06-python-docs.png) ·
  [Destino do Google](../build/browser-usability/18-google-link-redirecionamento.png)
- **Rolagem pelas setas.** Duas pressões de ↓ deslocaram o conteúdo em 192 px.
  [Captura](../build/browser-usability/05-python-setas.png)
- **Página inicial do Google por HTTP.** O navegador exibiu o logotipo e os
  controles de busca. Os valores internos de campos ocultos que apareciam na
  captura anterior não apareceram nesta sessão. [Captura](../build/browser-usability/09-google-http.png)

## Limitações observadas no Luna Browser e no LunaOS

| Área | Observação | Causa ou evidência | Status |
| --- | --- | --- | --- |
| Foco e digitação em formulários | Depois de clicar na área de busca do Google e em **Quick search** na documentação, `python tutorial` e `asyncio` foram para a barra de endereço. O campo **Quick search** manteve o texto original. | As teclas imprimíveis são encaminhadas à barra. O caminho de entrada da página ainda não dá foco nem edita campos. [Google](../build/browser-usability/13-google-texto-na-barra.png) · [Quick search](../build/browser-usability/21-docs-quick-search.png) · [Teclado do navegador](../ui/desktop.c#L378) · [Entrada na página](../ui/web_view.c#L448) | pendente |
| Envio de formulário | **Pesquisa Google** e **Estou com sorte** não iniciaram navegação. | O teste de clique do litehtml devolve links com `href`. O caminho atual não aciona botões de envio nem serializa os campos do formulário. [Pesquisa Google](../build/browser-usability/10-google-botao-pesquisa.png) · [Estou com sorte](../build/browser-usability/11-google-estou-com-sorte.png) · [Detecção de cliques](../ui/litehtml_bridge.cpp#L350) | pendente |
| Teclado virtual do Google | O clique no ícone de teclado não abriu o controle. | O clique não chega a um controle interativo pelo caminho atual. A execução de scripts externos também está ausente. A dependência específica do teclado virtual não foi isolada. [Captura](../build/browser-usability/12-google-teclado-virtual.png) · [Cliques](../ui/litehtml_bridge.cpp#L350) · [Scripts](../ui/web_js.c#L1311) | pendente |
| Busca pela barra de endereço | Pressionar Enter com `python tutorial` gerou **Invalid host**. | A barra trata o texto como destino de navegação. Na ausência de um esquema, acrescenta `https://` e pode tentar `http://` em seguida. Não há encaminhamento de termos a um buscador. [Captura](../build/browser-usability/14-google-texto-enter.png) · [Carregamento](../ui/desktop.c#L256) | pendente |
| Scripts do Python.org | No primeiro teste, a página exibiu a faixa vermelha `#nojs`. | A página atual baixa scripts clássicos e inicializa Modernizr, jQuery e Masonry. A faixa desapareceu no reteste por HTTPS. A compatibilidade com APIs usadas por outros scripts segue parcial. | **Faixa corrigida** |
| Menu da documentação | O painel lateral sobrepôs o conteúdo e permaneceu aberto após clicar no ícone do menu. A caixa **Quick search** também apareceu larga e cortada à direita. | O menu usa um controle de formulário e lógica em [menu.js](https://docs.python.org/3/_static/menu.js). O Luna passou a baixar o script. A ativação do controle pelo clique ainda está pendente. A causa exata do tamanho da caixa de busca não foi isolada. [Antes](../build/browser-usability/19-docs-antes-menu.png) · [Depois](../build/browser-usability/20-docs-depois-menu.png) | pendente |
| Âncoras na mesma página | Clicar em **Menu** no Python.org mudou o endereço para `#site-map`, recarregou a página e deixou a rolagem no topo. | O navegador resolve o fragmento como endereço e chama o carregamento completo, sem saltar para o elemento de destino. [Captura](../build/browser-usability/02-python-menu.png) · [Navegação por links](../ui/desktop.c#L481) | pendente |
| Consistência do layout após recarga | Na recarga provocada por **Menu**, o layout do Python.org passou para o renderizador legado. O conteúdo continuou disponível, com a navegação visualmente diferente. | Duas causas, ambas medidas nesta rodada: o log registrava `Browser: litehtml render deadline exceeded` e, com 64 MiB, `Browser: litehtml yielded to the low-memory reserve`. Nos dois casos o navegador caía no motor legado e mantinha essa queda para o resto da sessão. Ver [Memória do renderizador](#memória-do-renderizador). [Antes](../build/browser-usability/01-python-inicio.png) · [Depois](../build/browser-usability/02-python-menu.png) · [Log](../build/browser-usability/transcript.log) · [Prazo do renderizador](../lib/cxx_runtime.cpp#L43) | litehtml preservado em duas cargas com 4 GiB |
| `position: relative` em linha | No primeiro teste, o texto do botão **Donate** aparecia abaixo do fundo. | O deslocamento agora é aplicado uma vez ao fundo e ao texto. O teste repete o layout e verifica que ambos se movem 38 px. | **corrigido** |
| Documento grande | A busca avançada do Google foi recusada com **Chunked document exceeds the destination size limit**, e a página não abriu. | O destino da resposta tinha 128 KiB fixos, e o buffer de recepção recusava a resposta acima de 520 KiB. Ver [Documento grande](#documento-grande). | **corrigido** |
| Endereço com `&` no shell | `browse https://www.google.com/advanced_search?hl=pt-BR&autohuser=0` respondeu **lunaSH: unsupported operator**. | O shell tratava o `&` como operador e recusava a linha inteira; o `?` já era literal. Um `&` sozinho passou a fazer parte da palavra, e só o `&&` continua sendo o operador que junta dois comandos. | **corrigido** |
| Roda do mouse | Dois acionamentos da roda não alteraram a posição da página no QEMU. As setas funcionaram. | A roda não tinha caminho nenhum: o quarto valor do pacote VMMouse era descartado e não existia evento de rolagem na fila da GUI. [Após a roda](../build/browser-usability/04-python-roda.png) · [Após as setas](../build/browser-usability/05-python-setas.png) · [Entrada do mouse](../drivers/input.c#L214) | **corrigido** |
| Histórico | **Alt+←** manteve a documentação aberta. A interface não apresentou controles de voltar e avançar. | O navegador ainda não mantém histórico nem trata o atalho para voltar. [Captura](../build/browser-usability/07-voltar-alt-esquerda.png) · [Atalhos do navegador](../ui/desktop.c#L378) | pendente |
| Caracteres e ícones | Alguns textos e símbolos apareceram com `?`, como **Pesquisa avan?a** no Google e o símbolo ao lado de **Menu** no Python.org. | Três causas: a fonte embutida cobria só ASCII e Latin-1, o decodificador de UTF-8 tratava no máximo dois bytes por caractere e um documento que declara outra codificação chegava ao parser como bytes inválidos. As três foram corrigidas. Restou o caso do glifo que a fonte não tem: ele passou a ser desenhado como a caixa vazia de um navegador, e não como `?`. [Google](../build/browser-usability/09-google-http.png) · [Python.org](../build/browser-usability/01-python-inicio.png) · [Decodificação e glifos](../ui/graphics.c#L648) | **corrigido** |

## Respostas do Google e falha de transporte

Esses resultados foram observados também com a URL da busca digitada diretamente,
sem depender dos botões ou campos do Luna Browser.

- **Busca direta por HTTP.** `http://www.google.com/search?q=python+tutorial`
  exibiu uma página intermediária pedindo para clicar em **aqui** caso o
  redirecionamento não começasse. A navegação automática não ocorreu durante a
  espera observada. [Resposta inicial](../build/browser-usability/15-google-busca-direta-http.png) ·
  [Após a espera](../build/browser-usability/17-google-espera-redirecionamento.png)
- **Exigência de JavaScript pelo Google.** O clique em **aqui** abriu
  `/httpservice/retry/enablejs`, que pediu para ativar JavaScript para continuar
  pesquisando. Essa mensagem veio do Google. A etapa não foi concluída no Luna,
  e os resultados da pesquisa não apareceram. O mecanismo exato que a etapa
  exigia não foi isolado. [Captura](../build/browser-usability/18-google-link-redirecionamento.png)
- **Google por HTTPS.** Tanto a página inicial quanto a URL direta da busca
  falharam com `Transport interrupted; incomplete response rejected. TCP peer
  closed the connection.` O Luna registrou o fechamento TCP e rejeitou a
  resposta sem enquadramento HTTP completo antes de entregar conteúdo ao
  renderizador. O Python.org e sua documentação abriram por HTTPS na mesma
  sessão. [Página inicial](../build/browser-usability/08-google-inicio.png) ·
  [Busca direta](../build/browser-usability/16-google-busca-direta-https.png) ·
  [Fechamento TCP](../drivers/net.c#L1438) · [Validação da resposta](../drivers/net.c#L1581)
  — **corrigido**: o Google por HTTPS abre com logotipo, campo de busca e
  controles.

## Reteste da build corrigida

Sessão manual na build local em QEMU, sobre a imagem reconstruída depois das
correções descritas abaixo. A RAM do guest importa para o resultado: a 64 MiB
o renderizador do litehtml não comporta a página inteira do Python.org (ver
[Memória do renderizador](#memória-do-renderizador)), e a 256 MiB comporta. Por
isso as duas condições aparecem separadas nos itens abaixo.

| Item retestado | Resultado | Evidência |
| --- | --- | --- |
| Roda do mouse | A roda desloca o documento nos dois sentidos sobre o Python.org, mesmo com a barra de endereço em edição. Duas chavadas mudaram 352 das 640 linhas comparadas. | `dist/final-w-antes.png` · `dist/final-w-baixo.png` |
| Google por HTTPS | `https://www.google.com` abre com logotipo, campo de busca, **Pesquisa Google**, **Estou com sorte** e **Fazer login**. A falha de transporte não ocorre. | `dist/final-google.png` |
| Caracteres e ícones | **Pesquisa avançada** aparece com o `ç` correto. Uma página de prova mostra `→`, `←`, `↑`, `↓`, `•`, `—`, `…`, `✓`, `✗`, `«`, `»`, `★`, `☆` e `♥`. | `dist/final-google.png` · `dist/final-enc.png` · `dist/final-simbolos.png` |
| Codificação do documento | Uma página declarada como ISO-8859-1, que antes virava `avalia?`, é convertida e mostra `avaliação`. Uma página em UTF-8 mostra `ç`, `ã`, `á`, `é`, `ê` e `ó` em HTML literal, em entidade e em JavaScript embutido. | `dist/final-enc.png` |
| Documento grande | A busca avançada do Google, que ocupa 284 KiB, abriu com o formulário completo — **Pesquisa avançada**, **Fazer login** e os campos de busca — a 64 MiB e a 256 MiB. Antes, a página era recusada com `Chunked document exceeds the destination size limit`. | `dist/pyorg-probe.png` |
| Folhas de estilo grandes | O Python.org, com 350 KiB de folha de estilo, continua sendo desenhado com o CSS inteiro depois de a carga passar a usar o mesmo buffer que cresce. | `dist/pyorg-probe.png` |

## Correções aplicadas

**Transporte.** O pedido passou de HTTP/1.0 para HTTP/1.1, com `Accept` e
`Accept-Language`, e uma resposta sem `Content-Length` e sem
`Transfer-Encoding` deixou de ser tratada como truncada. O Google fechava a
conexão sem `close_notify`, o BearSSL reportava `BR_ERR_IO` e o validador
descartava a resposta por completo.

**Roda do mouse.** O quarto valor do pacote VMMouse traz a roda como o
movimento de um detente. O valor passou a ser lido e entregue como evento de
rolagem, com o sinal invertido conforme a convenção do QEMU.

**Texto.** A fonte embutida passou a cobrir ASCII, Latin-1, Latin
estendido-A e um conjunto de símbolos, com Noto Sans Symbols 2 como reserva
para os glifos que as famílias de texto não trazem. O decodificador de UTF-8
passou a aceitar sequências de três e quatro bytes, e um documento cujos bytes
não são UTF-8 válidos é lido como windows-1252 e convertido antes do parse.

**Glifo ausente.** Um caractere que nenhuma das famílias possui era
substituído por `?`, o que punia a página por um glifo que ela não pediu.
Passou a ser desenhada a caixa vazia que um navegador desenha, e o espaço
reservado na linha passou a ser o do próprio caractere ausente.

**Contêiner com float.** A largura de um contêiner que segura um float passou a
somar o conteúdo e o float que dividem a mesma linha, em vez de ficar com o
maior dos dois. Ver [a largura do contêiner](#float-a-largura-do-contêiner).

**Endereço com `&` no shell.** Um `&` sozinho passou a fazer parte da
palavra: o shell recusava a linha inteira de um endereço com consulta. Só o
`&&` continua sendo o operador que junta dois comandos.

### Documento grande

A busca avançada do Google ocupa 284 KiB e era recusada com `Chunked document
exceeds the destination size limit`. Não era um número de tamanho de página,
era um array fixo: `browser_body[131072]` em [ui/desktop.c](../ui/desktop.c) recebia o
corpo da resposta, e um segundo teto, o buffer de recepção de 520 KiB, recusava
a resposta inteira quando ela o estourasse.

O corpo passou a ser um buffer que cresce com o documento, e o buffer de
recebida também cresce em vez de recusar a resposta. O mesmo vale para as
folhas de estilo e as imagens, que tinham 512 KiB fixos. O teto que resta é de
4 MiB por documento, escolhido porque a memória do guest é contada: sem teto,
uma página enorme derrubaria o desktop inteiro em vez de mostrar um erro.

### Fontes

O Luna passou a oferecer três famílias de texto — Inter Tight, Noto Sans e
Montserrat — e o usuário escolhe a padrão em **Configurações → Appearance →
Typeface**. A escolha vale para a interface inteira e para as páginas, é
aplicada na hora e é gravada com as demais preferências. Noto Sans Symbols 2
não aparece na lista: ela existe apenas para fornecer os glifos que faltam nas
famílias de texto.

| Família | Licença | Papel |
| --- | --- | --- |
| Inter Tight | SIL OFL 1.1 | Padrão do sistema |
| Noto Sans | SIL OFL 1.1 | Alternativa |
| Montserrat | SIL OFL 1.1 | Alternativa |
| Noto Sans Symbols 2 | SIL OFL 1.1 | Reserva de símbolos |

O conjunto de glifos cobre 424 pontos de código: ASCII, Latin-1, Latin
estendido-A e os símbolos que a interface e os sites usam. Uma fonte de símbolos
não substitui as famílias de texto: ela só entra quando a família escolhida não
tem o glifo, e o glifo ausente em todas é desenhado como a caixa vazia de um
navegador.

A escolha das fontes aumentou o kernel para 5,0 MiB. A região reservada ao
kernel na imagem passou de 3,5 MiB para 8 MiB e o volume do sistema de arquivos
começou no setor 16384, de modo que o espaço do usuário ficou 4 MiB menor.

## Distância para o Firefox

Comparação do Python.org renderizado pelo Luna com o mesmo site no Firefox. A
diferença visível não é uma só: ela vem de quatro causas distintas, e só uma
delas é um defeito do renderizador.

| Causa | Sintoma | Situação |
| --- | --- | --- |
| Execução de JavaScript | A faixa vermelha `#nojs` fazia parte da versão exibida no primeiro teste. | **Corrigido** para a faixa. O Modernizr trocou a classe `no-js` por `js` e jQuery e Masonry foram carregados. |
| Ícones em fonte privativa | O ícone de busca e o glifo `≡` do menu vêm da fonte do site; nenhuma das nossas os tem. | **Corrigido**: o glifo ausente virou a caixa vazia de um navegador, no lugar do `?`. |
| Largura da janela | A janela do Luna tem cerca de 775 px de conteúdo; o CSS do site troca o campo de busca para 14 em acima de 940 px. | Comportamento correto, não defeito. |
| `float` no cabeçalho | O botão **Donate** aparece abaixo do painel de busca, e não ao lado dele. | **Corrigido**: a largura do contêiner não somava o float que divide a linha com o conteúdo. |

### O que o renderizador já faz

Medido com uma página de prova servida pelo host e carregada pelo Luna:

| Recurso | Resultado |
| --- | --- |
| `display: flex` | Funciona. |
| `@media (min-width: …)` | Funciona, dentro e fora de folhas externas. |
| Atributo `media` do `<link>` | Funciona, incluindo `all`, `screen` e `not print, braille, …`. |
| Cascata entre folha base e regra de mídia | Funciona: a regra de `@media` vence a base. |
| `position: absolute` | Funciona. |
| `::before` com `content` | Funciona. |
| `display: grid` | O litehtml desenha colunas com comprimentos, frações, `repeat()`, `minmax()`, espaçamento e itens que ocupam várias colunas. O teste verifica também `auto-fit` ao redimensionar. |

### `float`: a largura do contêiner

O Python.org posiciona o cabeçalho com `.site-headline { float: left }`,
`.options-bar-container { float: right }` e, dentro dele, o botão **Donate**
como conteúdo em linha seguido de `.options-bar { float: right }`. O Firefox
coloca o botão ao lado do painel de busca; o Luna o colocava abaixo dele.

A causa não era o posicionamento do float, e sim a **largura** do contêiner.
Uma página de prova reproduz o caso sem nenhuma regra do site: um
`float: right` de largura automática que contém um texto em linha e um
`float: right` interno. O litehtml dava ao contêiner a maior das duas
larguras, de modo que o float ocupava a largura toda e o texto ficava por
cima dele:

| | Antes | Depois | Firefox |
| --- | --- | --- | --- |
| Contêiner | 200 px | 248 px | 248 px |
| Float | 200 px, sobre o texto | 200 px, ao lado | 200 px, ao lado |
| Texto | 48 px, coberto | 48 px, ao lado | 48 px, ao lado |

O contêiner precisa caber o conteúdo e o float, que dividem a mesma linha, e
não o maior dos dois. A correção está em
[tools/litehtml-float-width.patch](../tools/litehtml-float-width.patch): a
caixa da linha passa a guardar a largura do float que divide a linha com ela.
O Makefile aplica os patches em uma cópia do litehtml antes de compilar. O
teste está em [tests/litehtml_float_test.cpp](../tests/litehtml_float_test.cpp)
e roda com `make test-litehtml` no CI.

### Memória do renderizador

Com 64 MiB de RAM, o Python.org **não cabe** no renderizador: o log registra
`Browser: litehtml yielded to the low-memory reserve` e o navegador apresenta
a página com o motor legado, sem CSS. As medições do guest:

| Momento | Memória livre |
| --- | --- |
| Depois do boot | 11.416 KiB |
| Depois de o litehtml construir o documento | 6.264 KiB |
| No momento da queda | 992 KiB |

O consumo é do litehtml: o parser de CSS legado do Luna custa 320 KiB no mesmo
ponto e o layout legado não custa nada além disso, ou seja, nenhum dos dois é a
causa. Com 256 MiB a página é desenhada com CSS completo e sem queda. Como a
reserva do renderizador é de 1 MiB e o sistema inteiro precisa dela, o caminho
curto é reduzir o consumo do litehtml, não aumentar a reserva.

O limite é o tamanho do DOM, não o tamanho do arquivo: a busca avançada do
Google, com 284 KiB, cabe a 64 MiB e sai com o CSS inteiro; o Python.org, com
52 KiB de HTML e 350 KiB de folha de estilo, não cabe. São os milhares de
elementos do cabeçalho e do mapa do site que pesam, não o texto.


### Reteste com scripts externos e 4 GiB

O teste HTTPS local abriu uma URL com `?` e `&` via `browse`. A página
executou scripts clássicos embutidos e externos em ordem, resolveu
um caminho relativo e continuou depois de um 404. O último script alterou
o DOM, e um temporizador confirmou a alteração depois de
`DOMContentLoaded`.

O litehtml deixou de contabilizar 96 MiB de alocações acumuladas por
operação. Ele reaproveita as arenas e cresce conforme as páginas físicas
livres, com uma reserva de 1 MiB para o sistema. No teste de heap,
uma alocação de 100 MiB passou e o descarte devolveu as páginas.
O prazo de CPU do layout passou a considerar o número de nós.

A IMG e a ISO chegaram ao desktop em QEMU UEFI x64 com 4 GiB. Nesse
perfil, o kernel i386 sem PAE identificou 3.139.376 KiB endereçáveis,
com aproximadamente 2.995.600 KiB de páginas livres no desktop inicial.
Duas cargas por HTTPS do Python.org mantiveram o litehtml ativo. O
layout levou cerca de 1,1 a 1,5 s no reteste final emulado. A faixa
`#nojs` desapareceu e o rótulo Donate ficou dentro do fundo amarelo.
[Captura](../dist/browser-python-uefi-4g.png)

Com 64 MiB, a cópia local da mesma página ainda usou o renderizador
legado. O log indicou que a próxima arena ultrapassaria a reserva do
sistema. Depois da queda, havia 1.492 KiB de páginas físicas livres.
