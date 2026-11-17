# Nova lógica de tiro da pistola (Duck Hunt) — especificação para o firmware

> **Estado:** implementado em `pistola.ino` e na ROM V5 (`src/patch_p2.s`), com os botões da tabela abaixo
> (A = luz, B = gatilho, Start/Select = acertos). Testado no simulador (`test_p2.py`, 46 casos, com uma
> pistola simulada que segue estas regras). Falta o teste na TV de verdade.
> A primeira versão da V5 (CRC da cópia `039228AF`) usava A = acertou e A + B = acertou o pato 2, sem a luz;
> ela não funciona com o firmware atual.

## Contexto
- Firmware: `pistola.ino` (ESP32-S3 Super Mini). Sensor TEPT4400 no GPIO1 (ADC, 10k para o GND), gatilho no GPIO2.
  A pistola é um gamepad HID por Bluetooth (BLE, ESP32-BLE-Gamepad + NimBLE) ou USB (USBHIDGamepad), conforme `USAR_BLUETOOTH`.
- Ela é o **controle 2** de um NES emulado (RetroArch no Recalbox, Raspberry Pi 3B+), ligado a uma TV de tubo
  (HDMI → conversor → VGA). Um quadro de TV = 16,67 ms. O emulador lê o controle 1 vez por quadro.
- Na V4: botão 1 (A) = "vejo luz" (contínuo, com `SEGURA_LUZ_MS` = 20 ms), botão 2 (B) = gatilho (pulso de 50 ms).
  O jogo decide o acerto olhando o A durante uma piscada longa (~220 ms).
- **Novo:** a pistola decide sozinha se acertou, olhando a sequência de luz que a TV mostra, e só avisa o jogo.
  O jogo (patch novo da ROM, feito à parte) mostra uma piscada curta e fixa e NUNCA espera a pistola:
  depois da piscada ele já conta erro, e só derruba o pato se chegar um "acertou" dentro de ~150 ms.

## Protocolo de botões (controle 2)
O jogo roda no emulador de NES; só a pistola se apresenta como um controle comum, com Start e Select
nos códigos padrão do Linux (o Recalbox passa A → A, B → B, Start → Start, Select → Select do NES).

| Pistola manda | Botão HID (código no Linux) | Quando | Significa |
|---|---|---|---|
| A (contínuo) | 1 (`BTN_SOUTH`, o de baixo) | enquanto o sensor vê luz (segura 150 ms) | "vejo luz". **A ROM ignora**: serve para a pistola funcionar como um controle comum (configurar no Recalbox) |
| B (pulso 50 ms) | 2 (`BTN_EAST`, o da direita) | ao puxar o gatilho | tiro; vale com ou sem A |
| Select (pulso 50 ms) | 11 (`BTN_SELECT`) | depois do tiro | acertou o pato 2 (Game B/C) |
| Start (pulso 50 ms) | 12 (`BTN_START`) | depois do tiro | acertou (Game A) / acertou o pato 1 (Game B/C) |
| nada | | | errou |

O Linux traduz o botão HID n de um gamepad no código `BTN_GAMEPAD + n - 1`. Nos botões 3 e 4 (a primeira
versão deste protocolo) o Start e o Select chegavam como `BTN_C` e `BTN_NORTH`, e o Recalbox não os via
como Start e Select.

- **Por que Start e Select:** o NES só tem A, B, Select, Start e direcional (X e Y não chegam ao jogo).
  O jogo original só lê o **direcional** do controle 2 (`$CA7C`: `LDA $17 / AND #$0F`, o pato do Game A);
  A, B, Start e Select do controle 2 nunca são lidos por ele. Por isso o direcional continua proibido.
- **Start e Select nunca saem juntos** (Select + Start fecharia o jogo no Recalbox).
- **Botões físicos Start e Select** (opcionais, GPIO4 e GPIO5 para o GND, ver `esquema_eletrico.png`)
  mandam os mesmos Start e Select: com eles, o Recalbox configura a pistola como um controle comum.
  O firmware ignora os dois **durante um tiro** (do gatilho ao fim do bloqueio): nos ~150 ms depois da
  piscada a ROM leria o aperto como "acertou".
- **O Hotkey da pistola pode ser o Select.** O firmware **nunca manda o A junto com o Select**: o A aperta
  sozinho com a luz, e Hotkey + A seria um atalho do Recalbox no meio do jogo. Select + Start (fechar o
  jogo) só acontece apertando os dois botões físicos. Quem preferir um Hotkey separado: 3 puxadas curtas
  no modo mapeamento mandam Home (HID 13), que a pistola nunca manda no jogo.
- Todos os botões vão **no mesmo relatório HID**:
  - BLE: `press()`/`release()` de cada botão e um `sendReport()` só (autoReport já está desligado).
  - USB: um envio único com a máscara completa (`gamepad.send(x, y, z, rz, rx, ry, hat, buttons)`).
- Cada pulso: segurar 50 ms (≥ 3 quadros, para o emulador não perder) e soltar.

## O que a TV mostra depois do tiro
A piscada começa ~3 a 6 quadros depois do gatilho (varia com USB/BT e o emulador). Contando a partir do
1º quadro preto (quadro 0):

    Game A:      0 P | 1 P | 2 Q | 3 Q | 4 P | 5+ cena normal
    Game B / C:  0 P | 1 P | 2 Q1 | 3 Q1 | 4 P | 5 Q2 | 6 Q2 | 7 P | 8+ cena normal

P = tela preta, Q = quadrado branco no pato (Q1 = pato 1, Q2 = pato 2). A pistola NÃO sabe se é Game A ou B;
as regras abaixo funcionam para os dois.

## Como a pistola enxerga isso
Na TV de tubo o feixe passa pelo ponto mirado 1 vez por quadro: quando o ponto está claro, o sensor dá
**um pulso curto por quadro** (~16,67 ms entre pulsos). Quadro escuro = pulso que não veio.
- Detecte pulsos como hoje (`leitura > linhaBase + DELTA_LUZ`), mas registre o **instante de cada pulso**
  (`micros()`, borda de subida, com ~3 ms de "surdez" depois de cada pulso para contar 1 por quadro).
- Não use `SEGURA_LUZ_MS` para decidir (ele só segura o A da luz); trabalhe com os instantes dos pulsos.

## Algoritmo por tiro (F = 16,67 ms)
1. **Gatilho** (borda, com debounce): manda o pulso de B. Guarda `tGat`. Entra no estado TIRO.
   Enquanto estiver em TIRO e no bloqueio (passo 6), **ignore novos gatilhos**.
2. **Achar o início do preto (`t0`)**. "Ida" = `t0 - tGat`. Ela inclui o pulso do gatilho (o jogo só
   registra o tiro quando B é solto), então é ~150 ms, não ~70.
   - **Pelo céu:** o último pulso foi seguido de 2 quadros sem pulso (um quadro preto confirmado
     por mais um). `t0` = último pulso + F. Aceite só se `t0 > tGat + 3F` (depois do pulso do gatilho) e:
     - se ainda **não há nenhuma medição**: aceite (é assim que a ida é aprendida);
     - se **já há medição**: só se `t0 - tGat` estiver entre `ida - 2F` e `ida + 3F`.
       Sem isso, o pato passando sob a mira (corpo escuro) parece o início do preto.
     Ao aceitar, guarde a medição (média das últimas 8).
   - **Pela estimativa** (ponto mirado escuro, ex. corpo do pato): só se **já houver medição**, quando
     passar `tGat + ida + 5F` sem sincronia pelo céu: `t0 = tGat + ida`, e avalie os pulsos guardados
     no histórico (olhar para trás).
     **Confira os quadros 0 e 1:** se houver luz neles, a estimativa está errada (e essa luz seria o céu).
     Então NÃO mande nada (um erro falso é melhor que um acerto falso) e **esqueça as medições**
     (zere a contagem E a posição da lista): no próximo tiro com o ponto claro, a ida é reaprendida.
     Isso cobre a troca de USB para Bluetooth (a ida muda).
3. **Janela 1** = pulsos entre `t0 + 1,5F` e `t0 + 3,5F` (quadros 2-3).
   No **primeiro pulso** dentro dela → manda **Start** imediatamente (acertou / pato 1). Fim da decisão.
4. **Janela 2** = pulsos entre `t0 + 4,5F` e `t0 + 6,5F` (quadros 5-6), **e** nenhum pulso entre
   `t0 + 6,5F` e `t0 + 7,5F` (quadro 7 escuro). Só depois de confirmar o quadro 7 escuro → manda **Select**
   (pato 2). A confirmação é obrigatória: no Game A a cena volta no quadro 5, e se a mira estiver no céu a
   luz é longa (não para no quadro 7). Isso NÃO é acerto.
5. Se nenhuma janela deu acerto até `t0 + 7,5F` → **não manda nada** (errou).
   Luz fora das janelas (céu voltando) é sempre ignorada.
6. **Bloqueio**: só saia do estado TIRO (aceitar novo gatilho) depois de `t0 + 8F + 160 ms`.
   Se nunca sincronizou: saia 600 ms depois de `tGat` (o jogo pode ter ignorado o tiro, por exemplo sem balas).
7. Nunca mande Start ou Select depois de `t0 + 8F` (o jogo só aceita até ~150 ms depois da piscada).

## Problema conhecido: sincronia pelo céu adiantada
A regra "último pulso + 2 quadros sem pulso = começou o preto" não distingue o preto da piscada de um
escurecimento do próprio ponto mirado. Mirando no **corpo do pato**, o pixel escurece e acende com a
animação; se ele escurece 1 ou 2 quadros antes do preto de verdade, `t0` sai 1 ou 2 quadros adiantado
(o filtro "perto da ida medida", de -2 a +3 quadros, aceita). Então:
- adiantado 1 quadro: o quadrado do pato 2 cai nos quadros 6-7, o "quadro 7 escuro" falha e o acerto se perde;
- adiantado 2 quadros, no Game B mirando no pato 1: o quadrado do pato 1 cai nos quadros 4-5 e o do pato 2
  (escuro, fora da mira) no 7, então a pistola manda **Select e derruba o pato errado**.

Medido no simulador (pixel único, o pior caso; o sensor de verdade vê uma área), 36 tiros no centro do pato
em cada versão: a V5 antiga (A / A + B) perdeu 2; a atual perdeu 3 e derrubou o pato errado 1 vez. A causa é
a mesma nas duas (a regra de sincronia não mudou). A sincronia "estimada" nunca errou.
Ainda não corrigido; os testes de Game B/C miram num canto do quadrado (céu) para testar o protocolo sem
depender disso.

## Diagnóstico pelo serial (115200)
A cada tiro, uma linha com: tipo de sincronia (céu / estimada), `t0 - tGat`, idaEstimada,
quadros (relativos a `t0`) dos pulsos vistos, decisão (Start, Select, nada) e o instante do envio.
Um comando `'t'` liga um **modo teste**: faz tudo, mas não envia Start nem Select, só registra.

## Como testar
- Mirando no céu em modo teste: a sincronia "céu" precisa achar `t0` em todos os tiros,
  e `ida` deve ficar estável (variação < 1 quadro no USB).
- Game A no pato: Start sai ~1 quadro depois do início do quadrado. No céu: nada.
- Game B: pato 1 → Start; pato 2 → Select ~3 quadros depois de `t0 + 5F`; céu → nada.
  No Game A mirando no céu, NUNCA pode sair Select.
