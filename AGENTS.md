- Confira os itens do [relatório de teste do Luna Browser](docs/browser-usability-test.md). Só remova esta linha após retestar todos os itens e confirmar que estão corretos e funcionais. Registre a conclusão como “problemas resolvidos”.

---

#: 9
Item do relatório: Roda do mouse
Causa raiz identificada: O pacote VMMouse traz 4 palavras; r[3] (scroll) é lido e descartado. O PS/2 só mapeia 3
  botões
Local: drivers/input.c:50-64
────────────────────────────────────────
#: 1
Item do relatório: Foco/digitação em campos
Causa raiz identificada: web_key_press() é um stub que retorna false; todo caractere printable vai para
  browser_url
Local: ui/web_view.c:448, ui/desktop.c:395
────────────────────────────────────────
#: 2
Item do relatório: Envio de formulário
Causa raiz identificada: web_click() só retorna nós com href — input/button nunca são detectados
Local: ui/web_view.c:420-446
────────────────────────────────────────
#: 3
Item do relatório: Teclado virtual do Google
Causa raiz identificada: Consequência de #2 (o clique não chega ao controle) + #5 (sem JS)
Local: idem
────────────────────────────────────────
#: 4
Item do relatório: "Invalid host" na barra
Causa raiz identificada: Texto sem esquema vira https://python tutorial e o espaço quebra o host; não há motor de
  busca
Local: ui/desktop.c:256-273
────────────────────────────────────────
#: 5
Item do relatório: Scripts do Python.org
Causa raiz identificada: <script src> é ignorado explicitamente (continue) — nunca é baixado
Local: ui/web_js.c:1312-1314
────────────────────────────────────────
#: 6
Item do relatório: Menu da documentação
Causa raiz identificada: Depende de menu.js (externo) e de position:fixed; sem JS o painel não fecha
Local: ui/web_js.c, CSS
────────────────────────────────────────
#: 7
Item do relatório: Âncoras #site-map
Causa raiz identificada: resolve_link transforma #site-map em URL absoluta → recarrega a página inteira
Local: ui/desktop.c:484-487
────────────────────────────────────────
#: 8
Item do relatório: Layout muda após recarga
Causa raiz identificada: Deadline de 1200 ms no renderizador → bad_alloc → litehtml_failed sticky, cai no motor
  legado para o resto da sessão
Local: lib/cxx_runtime.cpp:27,43, ui/web_view.c:52-59
────────────────────────────────────────
#: 10
Item do relatório: Histórico
Causa raiz identificada: Não existe pilha de histórico; Alt+← não é tratado
Local: ui/desktop.c:378
────────────────────────────────────────
#: 11
Item do relatório: ? em "Pesquisa avan?a" e no ícone do Menu
Causa raiz identificada: Duas causas: (a) decode() só trata UTF-8 de 1–2 bytes → 3 bytes viram ?; (b) a fonte é
  subset Latin-1 (32–126, 160–255) → ▾ (U+25BE) e → (U+2192) não existem
Local: ui/graphics.c:648-659,671, tools/fontgen.py:9
────────────────────────────────────────
#: 12
Item do relatório: Busca Google HTTP
Causa raiz identificada: Página de redirecionamento via JS; o follow de 3xx existe (drivers/net.c:1826-1847) mas
  JS não roda
Local: drivers/net.c, ui/web_js.c
────────────────────────────────────────
#: 13
Item do relatório: Google HTTPS: "Transport interrupted"
Causa raiz identificada: Cliente envia HTTP/1.0 + Connection: close, sem Accept/Accept-Language. Sem
  Content-Length e sem chunked, o corpo é delimitado pelo fechamento → o validador rejeita (content_length < 0).
  BR_ERR_IO(31) é tratado como "fechamento limpo" e a resposta é descartada
Local: drivers/net.c:1746-1758,1583-1594

Achado mais forte do lado de rede: o pedido é HTTP/1.0, o que o Python.org tolera e o Google não. A ausência de Content-Length é legal em HTTP/1.0 e o código a trata como corruption.

Plano de correção

P0 — Rede (itens 13, 12) — risco alto, ganho alto
1. drivers/net.c:1748 → HTTP/1.1 + Connection: close (manter close evita o parsing de keep-alive), adicionar Accept: text/html,..., Accept-Language, Sec-Fetch-Mode básico.
2. drivers/net.c:1591 → aceitar corpo delimitado por fechamento: se content_length < 0 e não é chunked e o peer fechou, o resto do buffer é o corpo (não é corruption). Rejeitar apenas se content_length declarado e insuficiente.
3. Diagnosticar antes de assumir: log do status line + Content-Length/Transfer-Encoding + Location na resposta do Google, para confirmar a hipótese em vez de adivinhar.
4. Teste de regressão com um fixture HTTP local (tests/fixtures/) servindo: resposta sem Content-Length, chunked, 301 e corpo grande — determinístico, sem depender do Google no CI.

P1 — Entrada: roda e foco (itens 9, 1)
5. drivers/input.c:60 → ler r[3] (dz) do pacote VMMouse e emiti-lo como evento de scroll; rotear ao web_scroll_by(). No caminho PS/2, mapear bytes 4/5 (botões 4/5) para scroll.
6. Modelo de foco de página: web_view.c ganha web_focus_element(x,y) / web_set_focus(node) / web_focused_node(). desktop.c:395 passa a roteiar printable para o campo focado; Backspace/Enter também. web_key_press() deixa de ser stub.

P2 — Formulários (itens 2, 3)
7. web_click() passa a devolver o tipo do alvo (link / input / button / submit), não só href. Adicionar web_attr para value e um mapa de valores editados.
8. Serialização de <form>: subir até o <form> ancestral, coletar name/value dos campos, montar query string, GET na action (ou no próprio documento).
9. Enter num campo de texto dispara submit.
10. O item 3 (teclado virtual) fica dependente de #9 e de P5 — registrar isso explicitamente no relatório, não prometer antes.

P3 — Âncoras, histórico, busca (itens 7, 10, 4)
11. href começando com # → localizar o elemento por id/name na árvore nativa (WebNode já tem attrs e geometria) e rolar até y, sem novo fetch.
12. Pilha de histórico no browser (browser_history[], índice, back/forward), Alt+←/Alt+→ e botões voltar/avançar na UI.
13. Detecção de "é URL?" na barra: se não há esquema, não há ponto e há espaços → tratar como consulta e ir para um buscador (lite.duckduckgo.com/lite ou similar, que não exige JS). Elimina o "Invalid host".

P4 — Texto e codificação (item 11)
14. tools/fontgen.py → ampliar o subset (Latin-1 + Latin Extended-A + set de símbolos UI: →, ▾, —, ·, ✓, «»). Custo: +alguns KiB na imagem; medir contra o teto de 120 MB.
15. ui/graphics.c:648 → decode() completo para 3 e 4 bytes (U+0800+ e planos astrais).
16. Isolar a origem do ? em "avançada": ç (U+00E7) está coberto pela fonte e pelo decode de 2 bytes — o ? portanto vem do caminho de parsing, não da fonte. Instrumentar separadamente (charset declarado vs. real) antes de mexer.

P5 — JavaScript (itens 5, 6, 12)
17. ui/web_js.c:1312 → baixar <script src> (mesmo mecanismo de browser_load_stylesheets), decodificar e executar, na ordem do documento.
18. Auditar as APIs DOM expostas ao QuickJS (o relatório cita TypeError: not a function) — reduzir o gap para o que menu.js e o redirecionamento do Google usam.
19. position:fixed / overlay no CSS nativo — verificar suporte; o menu da documentação depende disso.

P6 — litehtml (item 8)
20. O deadline de 1200 ms (lib/cxx_runtime.cpp:27) estoura no guest em CPU emulada sem SIMD. Duas frentes: (a) não re-renderizar do zero — luna_lite_create faz createFromString + render a cada navegação; cachear o documento e reusar; (b) tornar o deadline adaptativo e não-sticky: hoje litehtml_failed (ui/web_view.c:59) desliga o litehtml para o resto da sessão, o que produz exatamente a troca de layout observada. Registrar telemetria de tempo de render para calibrar.
21. Verificar o orçamento de 96 MB (renderer_bytes_left) contra os 64 MiB do QEMU — "litehtml yielded to the low-memory reserve" pode estar mascarando o deadline.

Sequenciamento e critérios

Ordem: P0 → P1 → P2 → P3 → P4 → P5 → P6. P0 e P4 são isolados e de baixo risco; P5/P6 são os que podem regredir a seção "O que funcionou" do relatório.
