# LunaOS Motion Engine (v1)

O módulo `ui/motion.c` / `ui/motion.h` é um motor nativo de interpolação temporal, integrado ao compositor e **não ao firmware**. Ele não adiciona bibliotecas externas, frames armazenados, texturas ou alocação de heap.

## Integração

- O kernel oferece `frame_clock` (ms, PIT a 1 kHz); a função `gui_motion_poll()` é executada no loop do kernel, antes da composição. O compositor mantém sua política de pacing e descarte de frames atrasados.
- Cada app tem um canal de interpolação fixo; são sete canais de estado pequeno. `motion_begin(id, now, from, to, dur_ms)` inicia/reinicia uma transição; `motion_value(id, now)` usa *smoothstep* Q10 inteiro sem ponto flutuante; `motion_active()` e `motion_cancel()` controlam o ciclo.
- Ao abrir uma janela, o compositor aplica um deslocamento visual de **28 px / 180 ms**; ao restaurar, também. Ao alternar o foco, aplica **10 px / 130 ms**. A posição real e a superfície em cache não mudam: o compositor blita a superfície na posição transitória.
- Apenas os retângulos anterior e atual (incluindo a sombra) são invalidados. Não existe repaint contínuo quando nenhuma transição está ativa. O cursor segue seu caminho de atualização independente.
- Arrastar, minimizar, fechar, maximizar ou trocar resolução cancela a animação associada, mantendo semântica e geometria das janelas.
- `motion_spinner_phase(now)` oferece fase discreta de oito pontos (80 ms por etapa); o navegador mostra um indicador durante `browser_loading`, sem recursos gráficos adicionais.

## Limites / próximas melhorias

- **Esta etapa usa translação, não zoom real, blur, keyframes CSS, fade por alpha ou VSYNC.** Ela evita alocação de buffers adicionais de tela. O objetivo é priorizar fluidez em CPU e memória limitadas.
- **Operações síncronas do navegador ainda podem bloquear o loop e congelar o spinner.** Um indicador visual não torna uma operação de rede assíncrona. O próximo passo é separar downloads do ciclo de renderização.
- O alvo de 60 FPS é um orçamento de composição, **não garantia de 60 FPS** sob TCG/hardware lento, nem taxa física programável do monitor.
- O canal por app é deliberadamente restrito; futuros elementos animados precisam de slots com identificação/lifetime independentes e limites bem definidos.
- O tempo usa subtração `uint32_t` e continua válido ao dar a volta em `frame_clock` para intervalos curtos.

## Testar

```sh
make test-motion
make check
make test
```

`tests/motion.c` verifica limites, monotonicidade, cancelamento, reinício, temporização e overflow do contador. Testes de integração em QEMU e observação de FPS/frame-time em hardware ainda precisam ser feitos.
