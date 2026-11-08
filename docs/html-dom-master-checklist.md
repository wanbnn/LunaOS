# Luna Browser — checklist mestre HTML/DOM

**Escopo obrigatório:** 11 componentes, 44 verificações. Esta lista é o contrato de
trabalho da evolução de HTML e DOM do LunaOS. A inclusão de um recurso ou um
teste local aprovado **não** equivale à conformidade integral. Cada verificação
só poderá ser concluída com código, teste oficial e evidência de execução.

## 1. HTML Tokenizer
- [ ] Implementar os estados de tokenização definidos no HTML Living Standard.
- [ ] Tratar comentários, DOCTYPE, RAWTEXT, RCDATA e scripts em todos os estados.
- [ ] Implementar tratamento completo de erros e EOF.
- [ ] Validar com html5lib-tests de tokenização.

## 2. HTML Tree Construction (WHATWG)
- [ ] Implementar insertion modes e pilha de elementos abertos.
- [ ] Implementar active formatting elements e adoption agency algorithm.
- [ ] Implementar foster parenting para tabelas.
- [ ] Validar árvores geradas com html5lib-tests.

## 3. Recuperação de HTML malformado
- [ ] Recuperar corretamente tags abertas/fechadas em ordem inválida.
- [ ] Corrigir reconstrução de tabelas, listas e elementos aninhados.
- [ ] Preservar comportamento normativo de documentos incompletos.
- [ ] Comparar resultados com navegadores de referência.

## 4. Elementos e atributos HTML
- [ ] Completar regras de elementos HTML e atributos.
- [ ] Implementar parsing contextual e regras de namespaces.
- [ ] Completar semântica de elementos especiais e conteúdo inerte.
- [ ] Cobrir casos oficiais de parsing HTML.

## 5. Entidades HTML
- [x] Integrar tabela completa de referências nomeadas WHATWG (2231 entradas em `ui/web_entities.inc`; testes nativos: CI 42530255907).
- [ ] Implementar regras para entidades com e sem ponto e vírgula.
- [ ] Completar remapeamento de caracteres numéricos e controles.
- [ ] Testar ambiguidades em atributos e texto.

## 6. DOM Core
- [ ] Implementar Document como tipo de nó distinto.
- [ ] Completar Node, Element, Text, Comment e DocumentFragment.
- [ ] Implementar ownerDocument, isConnected e getRootNode.
- [ ] Completar coleções dinâmicas, namespaces e propriedades.

## 7. DOM Mutation APIs
- [ ] Corrigir removeChild para preservar nós removidos.
- [ ] Implementar replaceChild, replaceWith, before, after e append/prepend.
- [ ] Implementar normalize, adoptNode, importNode e inserção de fragmentos.
- [ ] Garantir referências JS válidas, exceções e ownership correto.

## 8. DOM Events
- [ ] Implementar fases capture, target e bubble.
- [ ] Completar Event, CustomEvent e opções de listeners.
- [ ] Implementar once, passive, AbortSignal e stopImmediatePropagation.
- [ ] Testar cancelamento, propagação e composedPath.

## 9. Shadow DOM
- [ ] Implementar slotting e distribuição de nós.
- [ ] Implementar retargeting de eventos e composedPath.
- [ ] Completar encapsulamento e modos open/closed.
- [ ] Implementar APIs de slots e integração de estilos.

## 10. HTML Templates
- [ ] Completar parsing contextual dentro de template.content.
- [ ] Implementar adoção, clonagem e importação conforme especificação.
- [ ] Implementar nested templates e regras de inertness.
- [ ] Validar com html5lib-tests e WPT.

## 11. SVG/MathML DOM
- [ ] Implementar integração completa de foreign content.
- [ ] Preservar capitalização normativa e namespaces de atributos.
- [ ] Implementar os integration points de HTML/SVG/MathML.
- [ ] Validar parsing e criação de elementos via createElementNS.

## Gate para merge de implementação integral
- [ ] Suites oficiais html5lib-tests e WPT integradas com evidências.
- [ ] Regressões nativas e sanitizers sem falhas.
- [ ] QuickJS/DOM, ISO e execução real no guest testadas.
- [ ] Falhas residuais identificadas, classificadas e sem afirmar 100% falsamente.

Este arquivo é mantido junto ao código para não depender da memória da conversa.

## Evidência da fase 2 (PR #4)
- Tabela de 2231 entidades nomeadas integradas e verificadas com testes nativos.
- `web_node_root`, `web_node_is_connected`, `web_node_replace_child`, `web_node_normalize` e bindings QuickJS para métodos básicos.
- Pipeline: https://github.com/wanbnn/LunaOS/actions/runs/42530255907 (testes com sanitizers e análise sintática de `ui/web_js.c`).
- *Ainda sem* html5lib-tests, WPT ou validação do guest/ISO; não marcar os demais critérios como completos.
