# Luna Browser — integração Lexbor e decisão sobre Blink

## Estado desta implementação

- **Fonte Lexbor:** https://github.com/lexbor/lexbor, tag `v3.0.0`,
  commit imutável `2ae88a1c6b5261830eff73ee12bb3cdf805f3cfe`.
- **Licença:** Apache-2.0, preservada em `third_party/lexbor/LICENSE`.
- **Incorporação:** submódulo Git em `third_party/lexbor`.
  Inicializar com `git submodule update --init --recursive`.
- **Parser:** `ui/web_lexbor.c` invoca o tokenizer/tree builder Lexbor,
  transporta a árvore gerada para `WebNode` (compatibilidade com Luna CSS,
  layout e QuickJS). Não é ainda um DOM Lexbor vivo.
- **Build:** `make LUNA_LEXBOR=1 lexbor-native` compila a biblioteca
  estática freestanding x86 de 32 bits; `make LUNA_LEXBOR=1 all` seleciona
  Lexbor no navegador ao compilar a ISO. `LUNA_LEXBOR=0` mantém o legado.
  A opção permanece **desligada por padrão** até o teste da ISO/guest.
- **CI:** `.github/workflows/browser-dom.yml` compila Lexbor em host
  e freestanding, testa árvore DOM e sanitizadores, e tenta linkar o kernel.

## O que Lexbor substitui de fato

Com `LUNA_LEXBOR=1`: tokenização HTML, tratamento de referências de caracteres,
recuperação de HTML malformado, tree construction, foster parenting,
insertion modes, templates e parsing básico de foreign content seguem a
implementação Lexbor. A árvore usada para estilo e interação é copiada
para `WebNode`; suas mutações **não** são sincronizadas com o Lexbor.

Não declarar conformidade integral DOM/WPT a partir do sucesso de um
conjunto pequeno de smoke tests. Executar html5lib-tests e WPT de forma
sistemática, inclusive no guest.

## Limitações e riscos atuais

1. `WebNode` não é uma interface `Document`/DOM especificada em WHATWG.
2. O adaptador copia elementos, atributos, text, comments, templates e
   namespaces HTML/SVG/MathML; ainda não espelha todas as interfaces.
3. JavaScript muta o `WebNode` e não uma árvore Lexbor autoritativa.
4. A integração de stylesheet e eventos ainda é a implementação própria.
5. O parser roda com privilégios do kernel. **Isolar conteúdo não confiável
   em espaço de usuário** antes de alegar segurança para navegação geral.
6. A conversão recursiva limita profundidade; HTML excessivamente profundo
   precisa de estratégia iterativa e limites de recursos testados.
7. Com o backend desligado, o parser artesanal continua ativo.

## Complementos Blink/Chromium — critério técnico

**Não copiar cegamente Blink.** Muitos módulos têm dependências intrínsecas
de C++, V8, Oilpan/WTF, scheduler, IPC, bindings e plataformas gráficas.
Há arquivos com licenças específicas (alguns derivados de WebKit possuem
avisos LGPL); auditar licença *por arquivo* antes de qualquer transplante.

Preferir aproveitar o *comportamento da especificação* e implementar
adaptações independentes em C onde necessário. Pontos candidatos para
estudo comparativo do Blink, com testes WPT correspondentes:

- DOM Events: fases capture/target/bubble, retargeting de ShadowRoot,
  `composedPath`, `once`, `passive` e cancelation.
- Shadow DOM: slot assignment, flattened tree, composed tree e retargeting.
- DOM bindings: longevidade de nós removidos, `MutationObserver`, collections
  dinâmicas e regras de exceção.
- HTML integration: `innerHTML` contextual e streaming em documentos.
- SVG/MathML: integração DOM/estilo/namespace e APIs relacionadas.

O Blink **não está embutido no LunaOS** nesta etapa; não há arquivos Blink
copiados neste PR. Sua adoção requer escopo por arquivo, compatibilidade de
build, revisão de licenças e testes concretos.

## Comandos de verificação

```sh
git submodule update --init --recursive
make test-webengine
make LUNA_LEXBOR=1 lexbor-native
make LUNA_LEXBOR=1 build/kernel.elf
make LUNA_LEXBOR=1 all
```

A última linha constrói a imagem/ISO usando Lexbor; não é evidência de
funcionamento real em máquina até executar smoke tests e navegação.

**Checklist mestre:** `docs/html-dom-master-checklist.md`.
