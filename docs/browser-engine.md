# Navegador moderno: requisito aceito, integração ainda pendente

DOM e CSS modernos são um requisito do Luna Browser. **A imagem 0.3 não implementa
esse requisito.** O leitor nativo de texto/HTTPS e o comando JavaScript não devem
ser tratados como substitutos de um motor web completo.

## O que foi entregue como infraestrutura

- Framebuffer nativo de 32 bits no QEMU/Bochs, compositor com superfícies em cache e
  cursor independente. A rotina de composição pode receber buffers de um futuro
  processo de conteúdo sem usar o motor web para desenhar o desktop.
- Contextos de tarefa, paginação e níveis de privilégio; ainda não há loader ELF
  geral, memória de processo dinâmica, threads POSIX e IPC de buffers compartilhados.
- Transporte TCP com fluxo, ACK/retransmissão básica e TLS 1.2 BearSSL, com CA, hostname
  e data verificados, entropia RDRAND e sem opção de ignorar certificados inválidos.
- Decodificação HTTP chunked e rejeição de mensagens incompletas. Limite atual de
  documento: 128 KiB; sem redirecionamentos, HTTP/2, compressão ou cache web completo.
- QuickJS 2026-06-04 para o comando `js`, com memória/stack e tempo de execução
  limitados. **Não expõe document, DOM, CSSOM, layout ou APIs de browser.**

## Avaliação de WebKit

WebKit é o alvo de avaliação para DOM, CSS/layout e JavaScriptCore. Não existe um
binário WebKit para LunaOS pronto para incorporar à imagem.

A [FAQ oficial do WPE](https://wpewebkit.org/about/faq.html) informa que o porte WPE
atualmente funciona em sistemas Linux. A API WPEPlatform integra displays, views,
buffers e input; ela não implementa todos os serviços de um sistema operacional novo.
Os [ports oficiais do WebKit](https://docs.webkit.org/Ports/Introduction.html) têm
adaptações de plataforma e ciclos próprios de releases e correções de segurança.

Os [requisitos de build atuais do WPE](https://github.com/WebKit/WebKit/blob/main/Source/cmake/OptionsWPE.cmake)
incluem GLib/GIO e threads, HarfBuzz/ICU, bibliotecas de imagens, libxml2, SQLite,
Soup3 e uma integração gráfica. Não basta compilar um launcher ou ter libwebkit no
computador hospedeiro. Bibliotecas que dependem de Linux não executam automaticamente
sobre o LunaOS.

## Trabalho necessário para uma integração real

1. Definir ABI para executáveis de conteúdo e validar a arquitetura alvo do engine.
   Os clientes atuais são pequenos programas embutidos, não um loader para WebKit.
2. Implementar alocação/mapas de memória de processo, proteção por região, threads,
   sincronização, IPC, handles e transferência de superfícies entre processos.
3. Portar runtime C/C++ e dependências reais do engine ao guest. Os objetos ISO-C/math
   de musl usados pelo QuickJS não fornecem uma camada POSIX funcional.
4. Implementar o backend Luna de event loop, rede, arquivos/cache, relógios, fontes,
   text shaping, imagens, input, clipboard, persistência e rendering do WebKit.
5. Integrar UIProcess/WebProcess, DOM/WebCore, CSS/layout e JavaScriptCore; depois
   segurança de origem, cookies, storage, fetch, navegação e certificados do browser.
6. Executar testes de conformidade, testes visuais e páginas reais dentro do LunaOS.
   Só então anunciar suporte a páginas modernas e medir frame pacing do conteúdo web.

## Aceitação e tamanho

`tests/web-platform/modern.html` é um primeiro fixture **ainda não executado no guest**
para Grid, Flexbox, variáveis CSS, :has(), color-mix(), Shadow DOM, eventos, mutações,
microtasks e requestAnimationFrame. Ele não é uma implementação desses recursos.
A integração deverá também executar subconjuntos relevantes de Web Platform Tests
no processo de conteúdo do guest. Rodar os testes num browser Linux do host não
comprova compatibilidade do LunaOS.

A imagem atual tem 32 MiB (incluindo fontes Wi-Fi e setup) e o limite solicitado continua sendo 120 MB.
O porte de WebKit e suas dependências ainda não foi construído; portanto não há uma
medição que garanta que o navegador completo caberá nesse limite. Essa medição deve
usar uma build de release e a imagem final, não o tamanho de um mock ou do launcher.

Estado atual: **infraestrutura parcial pronta; motor DOM/CSS não integrado**.

## Referência HueBrowser

Foi consultado o [HueBrowser do usuário](https://github.com/wanbnn/huebrowser),
incluindo [src/main.c](https://github.com/wanbnn/huebrowser/blob/main/src/main.c).
A estrutura da interface contém `GtkWidget`, `GtkNotebook`, `GtkEntry` e
`WebKitWebView`. `webkit_web_view_load_uri` e as APIs de histórico/abas podem guiar
os comandos do navegador Luna. O motor e seu runtime não estão contidos nesse
arquivo: são dependências externas da instalação Linux do HueBrowser.

`WebKitWebView` do WebKitGTK é um widget GTK. Manter essa API e retirar GTK exige
portar/reimplementar essa integração, ou escolher outro port WebKit e integrar a
UI Luna às suas APIs. Portanto o caminho sem GTK continua sendo um port WebKit
nativo; copiar `main.c` não disponibiliza o motor DOM/CSS no guest. Nenhum motor
WebKit ou binário do HueBrowser foi integrado nesta imagem.

## Correções incrementais do motor nativo (novembro de 2026)

O código em `ui/web_*.c` já contém um parser HTML, cascata CSS, layout básico e
bindings DOM parciais. Isso não equivale à integração WebKit descrita acima.
Foram corrigidos comprimentos fracionários px/em/rem/pt, margens assimétricas,
`width/height: auto`, seleção de blocos `@media` por screen/print e largura,
reavaliação dessas regras ao redimensionar, e vazamentos durante relayout e parsing.
Blocos `@supports` são ignorados enquanto não há avaliação de capacidades.

O motor nativo também recebeu floats diretos (left/right), clear, posicionamento
básico, border-box, limites de largura, clipping de overflow, alinhamento de texto
e preservação de whitespace em pre. A cascata usa a especificidade do seletor que
realmente casou e respeita !important frente a estilo inline normal. Comentários
CSS dentro de declarações e irmãos de texto em :first-child são tratados.
O documento sintético não corresponde ao seletor universal, evitando ativar hacks
antigos como `* html`. Esses recursos são parciais, não conformidade CSS completa.

Imagens PNG externas são baixadas e decodificadas pelo stb_image, com limite de
transferência, dimensões e memória decodificada por documento. O compositor pinta
RGBA com transparência, respeitando o recorte da janela. JPEG, SVG, WebP e imagens
de background ainda não são implementados.

O python.org foi aberto por HTTPS dentro do LunaOS no QEMU e o menu horizontal e
o logotipo PNG foram observados numa captura real do guest. Isso não garante a
aparência de um navegador completo: faltam Flexbox/Grid, um modelo completo de
posicionamento/floats e APIs DOM/JavaScript. Scripts externos ainda não são
executados; o aviso vermelho do site e a navegação condicionada ao Modernizr podem
permanecer. Não se injeta uma classe `js` nem se oculta o aviso artificialmente.

Teste de regressão no host (inclui verificação de memória):

```sh
make test-webengine
```
