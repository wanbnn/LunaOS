# Porte de drivers Wi-Fi Linux para LunaOS

É possível reutilizar os drivers sem usar o kernel Linux como kernel do LunaOS.
O porte funcional ainda não foi implementado. `drivers/wireless.c` é somente
identificação PCI: não inicializa o rádio, não carrega firmware, não procura redes
nem autentica. Nenhuma placa Wi-Fi é anunciada como funcional na imagem atual.

O alvo prioritário fornecido pelo usuário é RTL8723BE PCI `10ec:b723`, subsistema
HP `103c:804c`. O Bluetooth requer identificação USB separada.

## Matriz de 15 alvos

A lista completa com IDs e fontes upstream está em
[reference/wifi-targets.json](reference/wifi-targets.json). Inclui RTL8188EE,
RTL8723BE, RTL8723DE, RTL8821CE, RTL8822CE, MT7612, MT7615, MT7663, MT7921,
MT7922, Intel 7260, Intel 8265, Intel AX200, QCA9377 e BCM43602.
São alvos de desenvolvimento, não 15 drivers entregues, nem uma afirmação de
participação de mercado. IDs de subsistema, revisão, firmware e capacidades reais
precisam entrar no binding de cada porte; identificar por vendor/device basta
somente para diagnóstico.

## Dependências verificadas no RTL8723BE

O levantamento dos includes está em
[reference/rtl8723be-dependencies.json](reference/rtl8723be-dependencies.json).
Os arquivos upstream foram baixados para `.tools/reference/linux-wireless` para
inspeção, preservando os cabeçalhos originais. Eles não fazem parte do kernel compilado.

`rtl8723be/sw.c` registra operações que usam a base `rtlwifi`, `rtl8723com` e
`btcoexist`. `rtlwifi/pci.c` manipula `struct ieee80211_hw`, buffers `sk_buff`,
anéis DMA e filas. `wifi.h` inclui serviços Linux e mac80211. Portanto copiar a
pasta `rtl8723be` e acrescentar os `.c` ao Makefile não fornece os serviços exigidos.

O caminho é portar os seguintes serviços reais, preservando os erros de funções
não implementadas; stubs que retornam sucesso não são aceitáveis:

1. Acesso PCI por dispositivo e região, mapeamento MMIO, ordenação de acessos,
   máscaras/alocação DMA, interrupções PCI/MSI e tratamento de remoção/falha.
2. Alocação, locks, temporizadores, workers e filas/buffers com propriedade e
   lifetime definidos. O allocator do QuickJS não substitui uma API DMA.
3. Carregamento e validação do firmware correto para revisão e subsistema,
   efuse/EEPROM, calibração RF, reset e inicialização dos anéis TX/RX.
4. Serviços equivalentes a mac80211/cfg80211: canais/domínio regulatório,
   varredura, autenticação/associação, gerenciamento, agregação e estados de link.
5. WPA2/CCMP, EAPOL e gerenciamento de chaves; WPA3/SAE precisa implementação e
   validação próprias. Não enviar senha Wi-Fi como texto em quadros de dados.
6. Integração à pilha IP/DHCP/DNS e UI de redes, reconexão e coexistência BT.
7. Testes em hardware: scan, associação, tráfego bidirecional, perda/reconexão,
   erros de firmware, RF kill e limites de canal. Repetir por família/revisão.

Os serviços comuns podem ser compartilhados. A sequência de energia, formato de
descritores, firmware e protocolo de comandos continuam específicos do chipset.
Os drivers mantêm suas licenças upstream e as obrigações da redistribuição devem
acompanhar qualquer código efetivamente integrado.

Bluetooth precisa também de transporte USB funcional e HCI/L2CAP/SMP/SDP/GATT,
além dos perfis usados. A parte Wi-Fi PCIe não fornece esses protocolos.

## Estado de aceitação

Atualmente: identificação de alvos e [serviços nativos v1](driver-services.md)
implementados e testados com PCI EDU do QEMU: DMA, MMIO, INTx, memória, locks,
work/timers, pacotes e carregamento binário de firmware. A adaptação de `rtlwifi`,
o firmware específico e a camada MAC/autenticação continuam pendentes.
Um QEMU com RTL8139 testa Ethernet, não valida nenhum desses rádios físicos.
