# LunaSDK v1 — API de desenvolvimento dentro do LunaOS

A primeira versão do **LunaSDK** fornece programação **dentro do guest**:
um shell `dev`, aplicativos JavaScript `.ljs` editáveis com Notes e dispositivos
virtuais `.ldrv` acessíveis por scripts e pelo terminal. O formato e a API
recebem versão; o comportamento pode evoluir enquanto o LunaOS é experimental.

> **Limite importante:** `.ljs` não é um binário ELF ou um novo processo ring 3.
> QuickJS executa como serviço do kernel e ainda **não tem isolamento suficiente
> para código não confiável**. `.ldrv` são dispositivos **virtuais**, não drivers
> PCI/USB reais, não executam código arbitrário e não têm acesso a MMIO/DMA/IRQ.

## Seu primeiro aplicativo

No lunaSH:

```sh
dev
dev new hello
dev list
dev run /home/luna/hello.ljs
sync
```

Abra o arquivo `/home/luna/hello.ljs` no **Notes**, edite o JavaScript,
salve com `Ctrl+S`, e execute com `dev run /home/luna/hello.ljs`.
Em uma instalação, substitua `luna` pelo usuário da sessão.
`dev new` nunca substitui um arquivo que já existe.

O script gerado já exercita o SDK. Exemplo:

```js
console.log("Olá!", luna.version);
console.log("API", luna.apiVersion, "uptime", luna.uptime());
luna.fs.write("mensagem.txt", "Criado no LunaOS");
console.log(luna.fs.read("mensagem.txt"));
console.log(luna.fs.list("."));
luna.app.open("notes");
```

### API JavaScript exposta por `dev run`

| Função | Retorno | Observações |
| --- | --- | --- |
| `luna.version` | string | Versão do LunaOS |
| `luna.apiVersion` | integer | Versão da API; inicial = 1 |
| `luna.uptime()` | integer | Relógio monotônico do kernel, em ms |
| `luna.fs.read(path)` | string ou `null` | Arquivos de texto existentes no diretório do usuário |
| `luna.fs.write(path, text)` | boolean | Cria ou sobrescreve arquivo de texto; **não persiste automaticamente** |
| `luna.fs.list(directory)` | array ou `null` | Nomes de filhos diretos de um diretório |
| `luna.app.open(name)` | boolean | `home`, `shell`, `files`, `browser`, `settings`, `tasks`, `notes` |
| `luna.device.read(name)` | string ou `null` | Estado do dispositivo virtual carregado |
| `luna.device.write(name, text)` | boolean | Escreve no dispositivo virtual |

Caminhos relativos em `luna.fs` são relativos ao **diretório home**, não ao
`cwd` do lunaSH. A API restringe caminhos ao home, inclusive após normalizar
`..`. O LunaFS suporta arquivos de texto de até **8191 bytes** e até **64
entradas** (arquivos + diretórios); as alterações só sobrevivem ao reinício
após `sync`. Esta checagem de caminho **não é um mecanismo de isolamento
de processos**.

Scripts são limitados pela configuração do QuickJS (stack, memória e
interrupção por tempo). Operações síncronas, como I/O pesado, ainda podem
bloquear o desktop. O comando original `js 'código'` continua disponível
sem os objetos adicionais do LunaSDK.

## Criar um dispositivo virtual

```sh
dev driver new lamp register
dev driver load /home/luna/lamp.ldrv
dev driver list
dev driver read lamp
dev driver write lamp on
dev driver read lamp
dev driver unload lamp
```

Arquivo `lamp.ldrv`, editável pelo Notes:

```ini
LDEV1
name=lamp
type=register
value=hello
```

Os dispositivos são **declarativos e limitados**. Até 8 são mantidos em RAM,
com identificador de 1 a 24 caracteres alfanuméricos, `_` ou `-`.
Tipos suportados:

- `register`: armazenamento de uma string de até 127 bytes, leitura e escrita.
- `upper`: armazena a última string escrita convertida para maiúsculas ASCII.
- `counter`: contador inteiro não negativo; aceita `inc`, `dec`, `reset` (valor inicial `0`).

Arquivos `.ldrv` persistem no LunaFS após `sync`, mas os **dispositivos
carregados e seus estados em RAM não persistem**; recarregue-os no boot
seguinte. Não há carregamento de código executável ou controle de hardware.

Um script pode interagir com o dispositivo:

```js
console.log(luna.device.read("lamp"));
luna.device.write("lamp", "ativada");
console.log(luna.device.read("lamp"));
```

## Drivers de hardware (API nativa C)

Drivers físicos **não** podem ser compilados e carregados dinamicamente pelo
LunaOS v0.3. A API existente do kernel em
[`kernel/driver.h`](../kernel/driver.h) fornece serviços para
**drivers nativos compilados junto ao kernel**: PCI, DMA, MMIO, interrupções
INTx, work queues, timers, firmware e filas de pacotes.

Fluxo atual de desenvolvimento: escrever C (pode ser rascunhado no Notes),
transferir/editar o fonte em um host com GCC de 32 bits, adicioná-lo em
`drivers/` e chamar seu `*_init()` no kernel conforme a arquitetura;
recompilar a imagem e testar no QEMU/hardware apropriado. Isto envolve
**código privilegiado**, sem isolamento e sem suporte a módulos ELF. Não
execute código de hardware desconhecido ou não auditado.

Ver [Serviços para drivers](driver-services.md) e
[Arquitetura](architecture.md).

## Testes

```sh
make test-devkit
make check
```

`tests/devkit.c` verifica parsing `.ldrv`, leitura, escrita, contadores,
conversão, descarregamento e recusa de descritores malformados.
Os cenários `dev new`/`dev run`/`dev driver` no guest precisam também
ser verificados manualmente ou em um teste QEMU. A compilação ou o teste
de virtual devices **não confirma drivers físicos**.
