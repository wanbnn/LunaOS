# LunaSDK v2 — aplicativos, drivers e hardware real

O LunaSDK evolui em direção a uma API unificada do LunaOS: um mesmo
programa pode usar recursos do sistema e endpoints fornecidos por drivers
de hardware reais. **A conexão com hardware é real**, não uma simulação,
quando o dispositivo está fisicamente presente. Suporte a runtimes e
toolchains completos no guest permanece um trabalho separado.

## Estado atual por linguagem

| Linguagem | Escrever no Notes | Executar no LunaOS | Compilar no LunaOS | Hardware |
| --- | --- | --- | --- | --- |
| **JavaScript `.ljs`** | Sim | QuickJS com `dev run` | Não aplicável | PCI real read-only, áudio e endpoints de drivers C nativos |
| **Python `.py`** | Sim | CPython com `python`, MicroPython com `micropython` e `dev run` | Não aplicável | Contagem/consulta PCI, áudio e endpoints de drivers C |
| **C `.c`** | Sim | **Não**, sem loader geral | **Não** (CMake só no host por enquanto) | API privilegiada PCI/DMA/MMIO/IRQ em C compilado no kernel |

O LunaOS embarca CPython para WASI, MicroPython e QuickJS. A toolchain de
C e o CMake rodam no host.

## Ferramentas no lunaSH

```sh
dev
dev new hello js
dev run /home/luna/hello.ljs
dev new scanner py
dev run /home/luna/scanner.py
dev new mydriver c
dev list
dev hardware pci
dev hardware read 0 2 0 0 4
dev driver list
sync
```

Altere `/home/luna` para o diretório da sessão atual. `dev new` não
sobrescreve arquivos existentes. O editor Notes permite modificar as fontes.

### Hardware real: PCI

A API em [`kernel/lunasdk_hardware.h`](../kernel/lunasdk_hardware.h)
delegada a [`drivers/pci.c`](../drivers/pci.c) faz leituras de verdade
nos registradores PCI CF8/CFC do processador. Não usa listas de dispositivos
fixas nem mocks em produção.

```js
// arquivo example.ljs, via "dev run"
const n = luna.hardware.pci.count();
console.log("PCI devices:", n);
for (let i = 0; i < n; i++) {
  const d = luna.hardware.pci.get(i);
  console.log(d.bus, d.slot, d.function, d.vendor, d.device, d.classCode);
}
const d = luna.hardware.pci.get(0);
if (d) console.log(luna.hardware.pci.read(d.bus, d.slot, d.function, 0, 4));
```

`pci.count()` retorna o número de funções PCI detectadas.
`pci.get(index)` retorna um objeto com `bus`, `slot`, `function`,
`vendor`, `device`, `revision`, `classCode`, `subsystem`, `irq`
e `pin` ou `null`. `pci.read(bus,slot,function,offset,width)` lê
1, 2 ou 4 bytes alinhados da configuração PCI (até offset 255).
Parâmetros inválidos são rejeitados. **Não existe `pci.write()` para scripts.**

`luna.audio.beep(hz, duration_ms)` controla o subsistema de áudio do
LunaOS com limites 20–20000 Hz e 1–250 ms. A disponibilidade de som depende
do dispositivo/driver em uso. `luna.device.read/write(name)` funcionam
tanto com os dispositivos virtuais `.ldrv` quanto com os endpoints
publicados por um **driver físico em C**.

**Importante:** leituras de configuração PCI são read-only; o código C
privilegiado continua sendo responsável por reivindicar o dispositivo,
habilitar o BAR, mapear MMIO, instalar INTx, configurar DMA e liberar tudo
no momento adequado. Acesso arbitrário a PCI/MMIO vindo de JS/Python
permitiria travar o kernel ou corromper memória e foi deliberadamente
excluído da API de scripts.

### Exemplo real de hardware: QEMU EDU

[`kernel/lunasdk_edu.c`](../kernel/lunasdk_edu.c) é um driver C real,
integrado ao boot. Quando encontra o dispositivo educativo **PCI
`1234:11e8`**, ele tenta fazer claim, habilitar BAR0, mapear MMIO e
registrar o endpoint `qemu-edu`.

Para testar com um dispositivo físico emulado pelo QEMU:

```sh
make -j2 all
tools/run-qemu.sh -device edu
```

No lunaSH do LunaOS:

```sh
dev hardware pci
dev driver list
dev driver read qemu-edu
```

O valor lido por `qemu-edu` vem do registrador **MMIO offset 0**
do dispositivo real conectado à VM. Caso o dispositivo não esteja
presente ou o claim/map falhe, o endpoint **não é registrado**. O
endpoint é somente leitura e não pode ser removido com `dev driver unload`.

O QEMU EDU é um dispositivo virtualizado pelo hipervisor, mas o driver
usa **a mesma API de PCI/MMIO real** do kernel, não um mock de software.
Para outras placas físicas, o desenvolvedor escreve um driver C próprio
usando o mesmo contrato, implementa o controle específico do chip e
registra callbacks em `ldev_native_register()`.

### Driver C com endpoint para JS/Python

A API de kernel `kernel/devkit.h` oferece:

```c
typedef int (*LunaNativeRead)(void *ctx, char *out, size_t cap);
typedef int (*LunaNativeWrite)(void *ctx, const char *value);
int ldev_native_register(const char *name, LunaNativeRead read,
                         LunaNativeWrite write, void *context);
```

A função `ldev_native_register` só deve ser usada por drivers nativos
confiáveis; ponteiros/callbacks C não são passados para scripts.
O driver mantém ownership de seu dispositivo, recursos MMIO/DMA e
callbacks. A interface exposta ao script é textual, limitada a 127 bytes
na leitura padrão de `dev driver read` e com validação por driver.

Os `.ldrv` criados via `dev driver new NAME register|upper|counter`
continuam disponíveis como **dispositivos puramente virtuais**. Eles
não representam hardware. O registro fixo tem 8 slots para endpoints
virtuais e nativos; estados em RAM não persistem após reinício.

## Python no LunaOS

O comando `python` executa **CPython 3.13.9** no guest. `python3` e
`python3.13` são aliases. O interpretador roda sobre wasm3 e vem na imagem
com módulos Python da biblioteca padrão. Use uma VM com pelo menos
128 MiB de RAM para esse runtime.

```sh
python --version
python -c 'import json; print(json.dumps({"resultado": 42}))'
python app.py
```

O port WASI lê arquivos do LunaFS, importa módulos e captura a saída no
lunaSH. A raiz WASI é `/`. Use caminhos absolutos para ler arquivos dentro
de scripts. O limite de execução é 60 segundos. A entrada interativa,
escrita de arquivos pelo CPython, subprocessos, sockets e extensões
nativas ficam fora do port atual.

Use `micropython` ou `dev run` para os scripts do LunaSDK que usam
`luna`. O MicroPython 1.29.0 permanece disponível com heap de 2 MiB e
limite de execução de 3 segundos.

```python
import luna

print(luna.hardware.pci.count())
print(luna.hardware.pci.get(0))
print(luna.hardware.pci.read(0, 2, 0, 0, 4))
print(luna.device.read("qemu-edu"))
luna.audio.beep(660, 25)
```

Os scripts LunaPy existentes ainda podem usar `luna` diretamente.
`luna.hardware.pci.get(index)` devolve uma descrição textual ou `None`.
O port usa a API de hardware do LunaSDK e aceita scripts confiáveis.

### Ambientes por projeto

```sh
python -m venv .venv
venv activate .venv
python app.py
deactivate
sync
```

O `venv` do LunaOS cria um diretório de bibliotecas `lib` dentro do
ambiente. Enquanto ele está ativo, os comandos Python usam esse diretório no
lugar do `lib` do usuário. Os ambientes compartilham os interpretadores
embarcados. A ativação dura até `deactivate` ou até reiniciar o sistema.
`sync` grava os diretórios do LunaFS no disco.

### Pacotes com pip

```sh
python -m venv .venv
venv activate .venv
python -m pip install -r requirements.txt
pip list
pip show pacote
pip freeze
pip uninstall -y pacote
pip cache info
pip cache purge
sync
```

O `pip` do LunaOS instala wheels Python puro marcados
`py3-none-any` ou `py2.py3-none-any`. `pip install` aceita um caminho
LunaFS, uma URL HTTP(S) do wheel, `pacote` ou `pacote==versão` no PyPI.
Na consulta ao PyPI, o download passa por HTTPS e é verificado pelo
SHA-256 publicado no índice. Para um espelho com a mesma API JSON, passe
`--index-url URL` apontando para a base que precede
`/<pacote>[/<versão>]/json`.

Em `requirements.txt`, use um pacote ou wheel por linha. Linhas em branco
e comentários iniciados por `#` são aceitos. Caminhos relativos de wheels
partem da pasta do arquivo de requisitos. Se uma linha falhar, os pacotes
concluídos nas linhas anteriores continuam instalados. Sem um ambiente
ativo, os pacotes ficam em `~/lib`. O `uninstall` usa o registro que o
instalador grava dentro do ambiente.

O cache dos wheels obtidos pelo índice é compartilhado entre ambientes do
mesmo usuário. Use `pip cache dir`, `list`, `info`, `remove NOME` ou `purge`.
`pip install --no-cache-dir ...` desabilita o cache naquela execução.

A instalação é limitada a 4 MiB por wheel, 4 MiB descompactados, 512
entradas e 64 KiB por arquivo. A versão deve ser exata ou a mais recente
publicada no índice. Para wheels que declaram dependências, inclua as
dependências no arquivo de requisitos e use `--no-deps`. A compatibilidade
do código depende das APIs disponíveis no runtime escolhido. Distribuições de
código-fonte, extensões binárias e a geração de scripts de entrada do
wheel ficam fora do port atual.

## CMake e SDK C: compilação no host, integração no kernel

O projeto CMake em [`sdk/native/CMakeLists.txt`](../sdk/native/CMakeLists.txt)
compila **o driver EDU do LunaOS** como objeto freestanding i386, útil
como base para drivers terceiros:

```sh
cmake -S sdk/native -B build/lunasdk-native
cmake --build build/lunasdk-native
```

O objeto criado **não é executável no guest** e não deve ser copiado
como se fosse um módulo carregável. No estado atual, a integração de um
driver C exige adicioná-lo à build do kernel e chamar sua inicialização
no boot. Veja [`kernel/driver.h`](../kernel/driver.h) para PCI claim,
DMA, mapeamento MMIO, IRQ, firmware e filas.

Para suportar literalmente **CMake dentro do LunaOS**, ainda são
necessários: sistema de arquivos com binários de tamanho real, processos
com ABI executável, loader ELF/relocações, execução de código externo com
isolamento/capacidades, toolchain C compatível e porte do próprio CMake.
Apenas disponibilizar um arquivo `CMakeLists.txt` não resolve isso.

## Segurança e limitações

- **JS e MicroPython rodam em serviços de confiança do kernel.** Limites de
  memória/tempo reduzem riscos, mas **não constituem sandbox de segurança**.
  Não execute código de terceiros não auditado.
- Acesso PCI read-only pode expor detalhes de hardware; futuras ABI devem
  usar capacidades por processo/usuário e mediação de dispositivos.
- **Nenhuma API de script recebe escrita arbitrária em PCI, MMIO, PIO,
  DMA ou IRQ.** Só funções controladas por driver C podem alterar hardware.
- A API C privilegiada não implica compatibilidade com drivers Linux;
  nenhum carregamento dinâmico de código de terceiros foi implementado.
- Os testes host simulam respostas PCI para conferir o contrato. Para
  comprovar interação com um dispositivo, rode no LunaOS com QEMU EDU
  ou uma placa real e acompanhe logs e valores lidos.

## Testes

```sh
make test-lunasdk-hardware test-devkit
make test-cpython test-python test-pip
make check
```

Os testes de ABI usam mocks no host. `make test-cpython` verifica CPython,
sintaxe de Python 3.12, imports e ambientes no QEMU. `make test-python`
verifica MicroPython e o LunaSDK no guest.
`make test-pip` usa wheels de teste e um índice HTTP local.


## Aplicacoes instaladas no lunaSH

O shell procura nomes que nao sejam comandos internos nos diretorios de
`PATH`, por padrao `/bin:/usr/bin`. Instale o executavel ELF32 nativo com o
nome do comando desejado: `/bin/minha-app` pode ser invocado como
`minha-app argumento`, sem alias nem alteracao no codigo do shell.
Comandos internos continuam tendo precedencia.

Use `set PATH /bin:/usr/bin:/home/luna/bin` para incluir outro diretorio.
Entradas relativas sao resolvidas contra o diretorio atual e entradas vazias
(inclusive em `:/bin` ou `/bin:`) representam o diretorio atual. Caminhos
explicitos como `/opt/app` e `./app` independem de `PATH`. O Tab inclui os
arquivos com assinatura ELF encontrados em `PATH`, sem repetir nomes.

A execucao direta espera a aplicacao terminar e disponibiliza seu codigo de
saida em `$?`, com `&&`/`||` condicionados a esse resultado. HOME, PWD, PATH e
as variaveis locais exportaveis sao passados ao processo. `elfrun` continua
disponivel para lancamento assincrono e `elfwait` para espera explicita.
O loader continua limitado ao ELF32/ABI LunaOS; arquivos Linux ou outros
formatos nao se tornam compativeis por estarem no PATH. No estagio atual,
a descoberta de ELF nao exige bits de execucao, pois o loader ainda nao
implementa enforcement completo de permissoes.

Pipes e redirecionamentos de aplicacoes nativas ainda nao sao suportados pelo
shell: a tentativa e rejeitada antes de executar ou truncar o destino. Os
pipes e redirecionamentos dos comandos internos conservam o comportamento
anterior. Codigo 127 indica comando ausente, 126 indica falha no lancamento.


## Git e shell nativos

O build padrao inclui `/bin/git` (upstream 2.56.1) e `/bin/sh` (Dash 0.5.12).
O porte da musl fica isolado em `sdk/musl-luna`. Os aplicativos usam numeros
internos da musl que sao traduzidos para a ABI LunaOS, nunca enviados como
numeros Linux diretamente ao kernel.

A ABI nativa agora oferece clocks (29), entropia (30), fstat (31), sinais e
mascara (32/33), kill e retorno de sinal (34/35), fork (36), duplicacao de
FD (37), flags/close-on-exec (38), sessoes (39), espera por sinal (40),
fchmod/mtime de FD (41/42) e poll limitado a 64 entradas (43).
Offsets de arquivos regulares herdados ainda nao sao compartilhados como
POSIX; a semantica de pipes/stdio e testada no guest. Detalhes de uso e
limites do porte estao em [git-native.md](git-native.md).
