# Luna Browser HTML/DOM — conformidade e critérios de aceite

Este documento descreve o estado da PR #3. **Não há evidência de 100% de conformidade.**
O parser atual é um subconjunto da especificação WHATWG e a maioria dos itens abaixo
ainda depende de implementação e testes de referência. Não alterar a `main` sem
regressões e testes nativos aprovados.

| Componente | Estado | Critério para marcar completo |
|---|---|---|
| HTML Tokenizer | Parcial | Estados oficiais de tokenização e casos html5lib-tests de tokenização |
| HTML Tree Construction | Parcial | Insertion modes, stack of open elements, active formatting, adoption agency e foster parenting verificados |
| Recuperação de HTML malformado | Parcial | Fixtures html5lib-tests de tree construction sem regressões |
| Elementos/atributos HTML | Parcial | Atributos, namespaces, void, foreign content e regras WHATWG |
| Entidades | Parcial | Tabela completa WHATWG, pontos de código inválidos e semicolon rules |
| DOM Core | Parcial | Document/Node/Element/Text/Comment, relações e live collections corretas |
| DOM Mutation APIs | Parcial | append/insert/remove/replace, fragment insertion, adopt, clone, exceptions e ownership |
| DOM Events | Parcial | captura, target, bubbling, listeners options, cancelation, composed path |
| Shadow DOM | Pendente | ShadowRoot, slotting, retargeting, encapsulamento, event composition |
| HTML Templates | Pendente | DocumentFragment, template content, parsing contextual e inertness |
| SVG/MathML DOM | Pendente | Namespaces, integração foreign content e atributos específicos |

## Alterações desta PR

- `web_node_text` não perde os primeiros filhos.
- Entidades numéricas acima de U+FFFF e caracteres inválidos são tratados.
- Fechamento de texto raw/RCDATA reconhece maiúsculas e espaços.
- RCDATA em `title`/`textarea` decodifica entidades.
- Atributos duplicados usam o primeiro valor e booleanos recebem string vazia.
- Árvores não aceitam ciclos; detach/insertBefore são centralizados.
- Fechamentos implícitos adicionais são suportados.
- Comentários HTML geram nós Comment no DOM.
- `insertBefore` está exposto nos bindings JS.

## Validação obrigatória

```sh
make test-webengine
make check
```

Depois, integrar `html5lib-tests` e Web Platform Tests com execução real no guest;
comparar árvores DOM serializadas com os resultados esperados dos fixtures.
Executar ASan/UBSan dos testes host, testes QEMU e testar a ISO em hardware físico.
Nenhum desses testes foi executado nesta sessão por falta de acesso de rede
no ambiente de compilação. A revisão da PR é obrigatória antes do merge.

## Limitações de arquitetura

O `WebNode` atual é uma árvore simplificada que não diferencia Document,
DocumentFragment, namespace e ShadowRoot em tipos apropriados. A implementação
inteira de WHATWG/DOM requer mais que extensões incrementais; precisa de tokenizer
por estados, tree builder por insertion modes e um modelo de propriedade/tempo de vida
dos nós que respeite referências JavaScript após `removeChild`.
A implementação existente de `removeChild` ainda libera o nó removido e não
atende o contrato DOM. Isso é um bloqueador para declarar conformidade.

## Iteração de 8 de novembro de 2026

Esta PR agora contém o modelo **inicial** de DocumentFragment, ShadowRoot,
`attachShadow({mode})`, `element.shadowRoot`, `template.content`,
`document.createDocumentFragment()`, `Node.cloneNode(deep)`, inserção
de filhos de DocumentFragment, namespaces básicos de SVG e MathML e fixtures
novos. A CI `browser-dom.yml` executa `make test-webengine` com
AddressSanitizer e UndefinedBehaviorSanitizer no GitHub Actions.

**Não confundir o modelo inicial com implementações completas:** falta o
algoritmo WHATWG de tokenizer e tree construction; os atributos SVG/MathML e
integration points ainda são incompletos. ShadowRoot não oferece slotting,
retargeting, `composedPath`, isolamento de estilos nem `adoptedStyleSheets`.
Templates ainda não oferecem HTML fragment parser conforme especificação,
clonagem/importação e adoção totalmente conformes. DOM Events e vida útil dos
objetos após remoção ainda precisam ser reformulados, inclusive porque
`removeChild` libera nós que o JavaScript deveria continuar podendo referenciar.
O binding JS e a ISO não receberam validação completa neste pipeline.

**Bloqueio de merge:** a intenção do PR original foi ampliada e a meta exigida
é conformidade integral. Não fazer merge como se HTML/DOM estivessem 100%.
Rodar WPT, html5lib-tests, `make check` e o browser no guest antes
de promover o conjunto a release.
