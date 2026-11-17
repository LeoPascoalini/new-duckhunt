/*
  Revolver do Duck Hunt - firmware da pistola (ROM V5: a pistola decide o acerto)
  Placa: ESP32-S3 Super Mini  (Arduino IDE: "ESP32S3 Dev Module", Flash Size "4MB",
         USB CDC On Boot "Enabled")

  A pistola aparece no PC/Raspberry como um CONTROLE (gamepad), no controle 2.
  Protocolo (ver PROTOCOLO_NOVO.md), como um controle comum (o jogo continua no emulador de NES):
    A      (HID 1  = BTN_SOUTH, o de baixo)   = luz no sensor, continuo (a ROM V5 ignora; serve
                                                para a pistola funcionar como um controle comum)
    B      (HID 2  = BTN_EAST, o da direita)  = gatilho (pulso de 50 ms)
    Select (HID 11 = BTN_SELECT)              = acertou o 2o alvo (Game B/C), pulso de 50 ms
    Start  (HID 12 = BTN_START)               = acertou (Game A) / acertou o 1o alvo, pulso de 50 ms
    nada                                      = errou (o jogo ja conta erro sozinho)
    Start e Select nunca saem juntos (Select + Start fecharia o jogo no Recalbox).
    Os botoes fisicos Start e Select (opcionais) mandam os mesmos Start e Select: servem para
    configurar no Recalbox e para os menus do Recalbox. Durante um tiro eles ficam bloqueados
    (o jogo leria o aperto como "acertou").
    A pistola olha a piscada da TV: 2 pretos, 2 quadros de quadrado, 1 preto
    (Game B/C: + 2 quadros do 2o quadrado e 1 preto) e decide sozinha.
  Precisa da ROM V5 com Start/Select (dist/duckhunt_pistola_p2.ips, CRC da copia 852EA4B2).
  (A ROM V4, com A = luz continua, usa o firmware do commit 12b6aca com PROTOCOLO 1.)

  Ligacoes (iguais ao esquema eletrico do projeto):
    GPIO1 (ADC1) <- emissor do TEPT4400 + resistor 10k para o GND
    GPIO2        <- microswitch do gatilho (outro terminal no GND)
    GPIO3 (ADC1) <- divisor 100k/100k das pilhas (opcional)
    GPIO4        <- botao Start  (outro terminal no GND; opcional)
    GPIO5        <- botao Select (outro terminal no GND; opcional)

  Bibliotecas:
    Bluetooth: "ESP32-BLE-Gamepad" (lemmingDev) + "NimBLE-Arduino" (h2zero)
    Cabo USB : ja vem no pacote esp32 da Espressif
               (Ferramentas > USB Mode: "USB-OTG (TinyUSB)")

  Monitor serial (115200):
    "### SENSOR"        -> o sensor comecou a ver luz / ficou no escuro
                           (na TV de tubo, a luz chega em pulsos: perto de 60 por segundo)
    ">> A B Start Select" -> cada mudanca dos botoes e se foi enviada
    "### TIRO"          -> resumo de cada tiro: sincronia, ida, pulsos, decisao
    "### BLUETOOTH"     -> estado da conexao (desconectado / sem pareamento seguro / CONECTADO)
    Comandos: 'm' liga/desliga o modo mapeamento (para configurar o controle no Recalbox),
              'd' liga/desliga a linha detalhada do sensor (a cada 100 ms),
              '+'/'-' ajusta a sensibilidade,
              't' modo teste: decide e mostra no serial, mas nao manda Start nem Select,
              'b' reinicia em modo de gravacao (sem apertar BOOT/RST).
*/

#include <Arduino.h>
#include <stdint.h>

// ======================= CONFIGURACAO =======================
#define USAR_BLUETOOTH  1      // 1 = sem fio (BLE)   0 = cabo USB

const int PINO_SENSOR  = 1;
const int PINO_GATILHO = 2;
const int PINO_BATERIA = -1;    // 3 se montar o divisor 100k/100k; -1 = sem divisor (informa 100%)
const int PINO_START   = 4;     // botao fisico Start  (-1 = nao montado; sem botao o pino fica solto = nao apertado)
const int PINO_SELECT  = 5;     // botao fisico Select (-1 = nao montado)

int   DELTA_LUZ        = 300;   // quanto a leitura precisa subir acima da linha de base (0-4095)
// "Segura" a luz depois do ultimo pulso: sem luz por 150 ms = escuro (o preto do tiro dura ~33 ms,
// entao a piscada nao solta o A). Vale para o A e para o aviso "### SENSOR". Precisa ser bem maior
// que 1 quadro: na tela escura do Recalbox alguns pulsos falham, e o A nao pode soltar no meio (o
// Recalbox pede para SEGURAR o botao ~1 s e recomeca a contagem a cada soltura). Tambem limita as
// mudancas do A (cada uma e um envio Bluetooth) enquanto a mira passeia pela tela.
const uint32_t SEGURA_LUZ_MS   = 150;
const uint32_t DEBOUNCE_MS     = 5;     // filtro do microswitch
const uint32_t PULSO_B_MS      = 50;    // duracao do "aperto" de B a cada tiro (~3 quadros)
const uint32_t DORMIR_APOS_MS  = 10UL * 60UL * 1000UL;  // dorme apos 10 min parada
const float    BAT_CHEIA_V     = 4.6f;  // 3x AA alcalinas novas
const float    BAT_VAZIA_V     = 3.4f;  // abaixo disso o regulador nao segura 3,3 V
const float    BAT_CORRECAO    = 1.00f; // ajuste fino comparando com um multimetro

// Intervalo de conexao Bluetooth pedido ao Recalbox, em unidades de 1,25 ms.
// O Linux usa 30-50 ms se a pistola nao pedir nada; cada envio espera o proximo
// "encontro" de conexao, entao intervalo menor = botoes chegam mais cedo.
// 6..12 = 7,5..15 ms. Se a conexao ficar instavel (Pi 3B+ com Wi-Fi ligado), use 12..24.
const uint16_t BLE_INTERVALO_MIN = 6;
const uint16_t BLE_INTERVALO_MAX = 12;

// Deteccao do tiro (ver PROTOCOLO_NOVO.md)
const uint32_t QUADRO_US      = 16667;   // 1 quadro da TV (60 Hz)
const uint32_t PULSO_MSG_MS   = 50;      // Start / Select ficam apertados ~3 quadros
const uint32_t IDA_INICIAL_US = 150000;  // gatilho -> 1o quadro preto na TV, antes de medir
const uint32_t SURDEZ_US      = 3000;    // depois de um pulso do tubo, ignora ~3 ms (1 pulso por quadro)
const uint32_t BLOQUEIO_US    = 160000;  // depois de t0 + 8 quadros, ainda ignora o gatilho
// =============================================================

#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "soc/rtc_cntl_reg.h"

#if USAR_BLUETOOTH
  #include <BleGamepad.h>
  #include <NimBLEDevice.h>
  BleGamepad gamepad("Revolver Duck Hunt", "DIY", 100);
#else
  #include "USB.h"
  #include "USBHIDGamepad.h"
  USBHIDGamepad gamepad;
#endif

// ---- botoes fisicos Start / Select (com filtro, como o gatilho) ----
// (o struct vem antes de qualquer funcao: a IDE do Arduino poe os prototipos antes da 1a funcao)
struct BotaoFisico { int pino; bool bruto, estavel; uint32_t mudou; };
BotaoFisico fisStart = { PINO_START, false, false, 0 }, fisSelect = { PINO_SELECT, false, false, 0 };

// ---- botoes (numeracao HID: botao n = bit n-1 da mascara) ----
// O Linux traduz o botao HID n de um gamepad no codigo BTN_GAMEPAD + n - 1:
//   1 = BTN_SOUTH (o de baixo), 2 = BTN_EAST (o da direita), 3 = BTN_C, 4 = BTN_NORTH, 5 = BTN_WEST,
//   7/8 = L/R, 11 = BTN_SELECT, 12 = BTN_START, 13 = BTN_MODE (Home).
// Por isso Start e Select precisam ser o 12 e o 11 (no 3 e no 4 eles chegavam como "C" e "X").
const uint8_t BOTAO_A = 1, BOTAO_B = 2, BOTAO_SELECT = 11, BOTAO_START = 12;
const uint8_t BOTAO_HOTKEY = 13;  // so no modo mapeamento: Home, para o Hotkey (a pistola nunca manda)
inline uint32_t mascara(uint8_t botao) { return 1UL << (botao - 1); }
const char* nomeBotao(uint8_t n) {
  switch (n) {
    case BOTAO_A: return "A";            case BOTAO_B: return "B";
    case BOTAO_SELECT: return "Select";  case BOTAO_START: return "Start";
    case BOTAO_HOTKEY: return "Hotkey (Home)";
    default: return "?";
  }
}

// ---- estado ----
float    linhaBase   = 0;     // nivel de luz "normal" (ambiente + tela), acompanhado devagar
uint32_t ultimaLuz   = 0;
uint32_t saida       = 0;     // botoes que estao sendo mandados agora (mascara)
bool     gatilhoBruto = false;
uint32_t mudouGatilho = 0;
uint32_t ultimaAtividade = 0;
uint32_t ultimoDebug = 0, ultimaBateria = 0;
bool     debug = false;          // 'd' no monitor serial liga a linha detalhada a cada 100 ms
int      leituraMax = 0;

bool leBotao(BotaoFisico& b, uint32_t agora) {
  if (b.pino < 0) return false;
  bool g = digitalRead(b.pino) == LOW;
  if (g != b.bruto) { b.bruto = g; b.mudou = agora; }
  if (agora - b.mudou >= DEBOUNCE_MS && b.estavel != b.bruto) {
    b.estavel = b.bruto;
    ultimaAtividade = agora;
  }
  return b.estavel;
}

// ---------------- saida para o gamepad ----------------
// Todos os botoes vao num relatorio HID so.
// Envia PRIMEIRO e so depois escreve no monitor serial: a mensagem nunca atrasa o botao.
void enviar(uint32_t m) {
#if USAR_BLUETOOTH
  bool ok = gamepad.isConnected();
  if (ok) {
    static const uint8_t usados[] = { BOTAO_B, BOTAO_A, BOTAO_SELECT, BOTAO_START };
    for (uint8_t bt : usados)
      (m & mascara(bt)) ? gamepad.press(bt) : gamepad.release(bt);
    gamepad.sendReport();
  }
  const char* como = ok ? "enviado por Bluetooth" : "NAO ENVIADO (Bluetooth sem conexao HID)";
#else
  gamepad.send(0, 0, 0, 0, 0, 0, 0, m);               // um relatorio so, com todos os botoes
  const char* como = "enviado por USB";
#endif
  Serial.printf(">> A:%s B:%s Start:%s Select:%s | %s\n",
                m & mascara(BOTAO_A) ? "SIM" : "-  ", m & mascara(BOTAO_B) ? "SIM" : "-  ",
                m & mascara(BOTAO_START) ? "SIM" : "-  ", m & mascara(BOTAO_SELECT) ? "SIM" : "-  ", como);
}

// ---------------- MODO MAPEAMENTO (configurar o controle no Recalbox) ----------------
// Com os botoes fisicos Start e Select montados, o modo mapeamento nao e preciso: no modo normal
// ja da para mapear A (luz), B (gatilho), Start e Select (botoes), e o Hotkey pode ser o Select
// (o firmware nunca manda o A junto com o Select). Para um Hotkey separado: 3 puxadas = Home.
// Ligue a pistola SEGURANDO o gatilho por 3 s, ou digite 'm' no monitor serial. Nesse modo:
//   luz no sensor           -> A, apertado enquanto houver luz (igual ao modo normal)
//                              (segurar a luz ~1 s = "segurar um botao": inicia a configuracao
//                               ou pula um item; luz rapida, menos de 1 s = mapeia o item)
//   puxada LONGA (>1 s)     -> B
//   1 puxada curta          -> Start
//   2 puxadas curtas        -> Select
//   3 puxadas curtas        -> Home (HID 13), para o HOTKEY (a pistola nunca manda: assim o
//                              Select dela e so Select, nunca Hotkey)
//   (puxadas curtas seguidas = menos de 0,4 s entre uma e outra; o botao sai 0,4 s depois da ultima)
// Para voltar ao modo normal: 'm' de novo, ou desligue e ligue.
bool    modoMapa = false;
const uint32_t JANELA_PUXADAS_MS = 400;
const char* MSG_MAPA = "MODO MAPEAMENTO: luz = A | puxada longa = B | 1 puxada curta = Start | "
                       "2 = Select | 3 = Home (Hotkey) | luz por 1 s = pula o item ('m' volta ao normal)";

void pulsoBotao(uint8_t n) {
#if USAR_BLUETOOTH
  if (!gamepad.isConnected()) {
    Serial.printf("[mapa] %s (botao HID %d) NAO ENVIADO (Bluetooth sem conexao HID)\n", nomeBotao(n), n);
    return;
  }
  gamepad.press(n);   gamepad.sendReport(); delay(250);
  gamepad.release(n); gamepad.sendReport();
#else
  gamepad.pressButton(n - 1); delay(250); gamepad.releaseButton(n - 1);
#endif
  Serial.printf("[mapa] %s (botao HID %d)\n", nomeBotao(n), n);
}

// =============================================================
//  TIRO: a pistola decide o acerto
// =============================================================
// Historico dos pulsos do tubo (instante em micros, 1 por quadro quando o ponto esta claro)
const int N_PULSOS = 64;
uint32_t pulsos[N_PULSOS];
int      iPulso = 0, nPulsos = 0;
uint32_t ultimoPulsoUs = 0;
uint32_t totalPulsos = 0;        // so para o aviso "### SENSOR"

// "Ida" = do gatilho ate o 1o quadro preto na TV. Media das ultimas medicoes.
uint32_t idaMedidas[8];
int      nIda = 0, iIda = 0;
uint32_t idaUs = IDA_INICIAL_US;

bool     modoTeste = false;      // 't': decide mas nao manda Start / Select
uint32_t msgStartAte = 0, msgSelectAte = 0;  // millis ate quando Start / Select ficam apertados

enum { SYNC_PROCURANDO = 0, SYNC_CEU = 1, SYNC_ESTIMADA = 2, SYNC_FALHOU = 3 };
enum { DEC_NADA = 0, DEC_ALVO1 = 1, DEC_ALVO2 = 2 };

struct Tiro {
  bool     ativo;
  uint32_t tGat;          // instante do gatilho (micros)
  bool     temT0;
  uint32_t t0;            // instante do 1o quadro preto na TV (micros)
  uint8_t  sync;
  bool     luzPreto, luzJ1, luzJ2, luzQ7;   // luz nos quadros 0-1, 2-3, 5-6 e 7 da piscada
  uint8_t  decidiu;
  uint32_t enviouUs;
  uint8_t  nRel;
  int16_t  rel[12];       // quadro (contado a partir de t0) de cada pulso visto, para o log
} tiro;

void guardaPulso(uint32_t t) {
  pulsos[iPulso] = t;
  iPulso = (iPulso + 1) % N_PULSOS;
  if (nPulsos < N_PULSOS) nPulsos++;
  ultimoPulsoUs = t;
  totalPulsos++;
}

void guardaIda(uint32_t ida) {
  idaMedidas[iIda] = ida;
  iIda = (iIda + 1) % 8;
  if (nIda < 8) nIda++;
  uint64_t soma = 0;
  for (int i = 0; i < nIda; i++) soma += idaMedidas[i];
  idaUs = (uint32_t)(soma / nIda);
}

// Em qual quadro da piscada caiu um pulso (0, 1 = preto; 2, 3 = 1o quadrado; 4 = preto;
// 5, 6 = 2o quadrado; 7 = preto) e marca as janelas.
void classificaPulso(uint32_t tp) {
  int32_t d = (int32_t)(tp - tiro.t0);
  if (d < -(int32_t)(QUADRO_US / 2)) return;
  int k = (d + (int32_t)(QUADRO_US / 2)) / (int32_t)QUADRO_US;
  if (tiro.nRel < 12) tiro.rel[tiro.nRel++] = (int16_t)k;
  if (k <= 1)                tiro.luzPreto = true;
  else if (k <= 3)           tiro.luzJ1 = true;
  else if (k == 5 || k == 6) tiro.luzJ2 = true;
  else if (k == 7)           tiro.luzQ7 = true;
}

void defineT0(uint32_t t0, uint8_t sync) {
  tiro.t0 = t0;
  tiro.temT0 = true;
  tiro.sync = sync;
  // olha para tras no historico: pulsos que ja chegaram depois de t0
  for (int i = 0; i < nPulsos; i++) {
    uint32_t p = pulsos[(iPulso - 1 - i + N_PULSOS) % N_PULSOS];
    if ((int32_t)(p - t0) >= -(int32_t)(QUADRO_US / 2)) classificaPulso(p);
  }
  // sem sincronia pelo ceu, so vale se onde devia haver preto estava mesmo escuro;
  // senao a estimativa esta errada (e a luz seria o ceu): nao arrisca acerto falso
  if (sync == SYNC_ESTIMADA && tiro.luzPreto) {
    tiro.sync = SYNC_FALHOU;
    nIda = 0;                 // a ida guardada nao serve mais (trocou USB/BT?): reaprende no proximo tiro
    iIda = 0;
    idaUs = IDA_INICIAL_US;
  }
}

void mandaMensagem(uint8_t tipo, uint32_t agoraUs, uint32_t agoraMs) {
  tiro.decidiu = tipo;
  tiro.enviouUs = agoraUs;
  if (modoTeste) return;
  if (tipo == DEC_ALVO1) msgStartAte  = agoraMs + PULSO_MSG_MS;
  else                   msgSelectAte = agoraMs + PULSO_MSG_MS;   // nunca os dois juntos
}

void comecaTiro(uint32_t agoraUs) {
  memset(&tiro, 0, sizeof(tiro));
  tiro.ativo = true;
  tiro.tGat = agoraUs;
}

void logTiro() {
  static const char* nomeSync[] = { "nenhuma", "ceu", "estimada", "FALHOU (luz onde devia haver preto)" };
  static const char* nomeDec[]  = { "nada (errou)", "Start (acertou / alvo 1)", "Select (alvo 2)" };
  Serial.printf("### TIRO | sincronia: %s | t0-gatilho: %.1f ms | ida estimada: %.1f ms (%d medicoes) | pulsos nos quadros:",
                nomeSync[tiro.sync], tiro.temT0 ? (int32_t)(tiro.t0 - tiro.tGat) / 1000.0f : -1.0f,
                idaUs / 1000.0f, nIda);
  for (int i = 0; i < tiro.nRel; i++) Serial.printf(" %d", tiro.rel[i]);
  Serial.printf(" | decisao: %s", nomeDec[tiro.decidiu]);
  if (tiro.decidiu) Serial.printf(" em t0+%.1f ms", (int32_t)(tiro.enviouUs - tiro.t0) / 1000.0f);
  Serial.printf("%s\n", modoTeste ? " | MODO TESTE (nao enviado)" : "");

  // dica do que provavelmente aconteceu
  if (tiro.sync == SYNC_PROCURANDO)
    Serial.println("    -> nao achou o inicio da piscada. Se a TV piscou: a pistola ainda nao sabe o atraso;"
                   " de 1 ou 2 tiros mirando em algo CLARO (ceu, logotipo do titulo). Se a TV nao piscou:"
                   " o jogo nao registrou o tiro.");
  else if (tiro.sync == SYNC_FALHOU)
    Serial.println("    -> havia luz onde devia haver preto: o atraso mudou. O proximo tiro em algo claro reaprende.");
  else if (tiro.decidiu)
    Serial.printf("    -> mandou %s. Se o pato NAO caiu, o jogo nao recebeu: confira se a ROM e a V5 com"
                  " Start/Select (CRC 852EA4B2) e se o %s da pistola esta mapeado no Recalbox.\n",
                  tiro.decidiu == DEC_ALVO1 ? "Start" : "Select", tiro.decidiu == DEC_ALVO1 ? "Start" : "Select");
  else
    Serial.println("    -> sincronizou, mas nao viu o quadrado nos quadros certos (2-3, ou 5-6 com o 7 escuro)."
                   " Mirando no ceu e o normal (errou). Mirando no pato: guarde esta linha.");
}

void cuidaDoTiro(uint32_t agoraUs, uint32_t agoraMs) {
  if (!tiro.ativo) return;
  const int32_t F = (int32_t)QUADRO_US;

  if (!tiro.temT0) {
    // 1) inicio do preto: o ultimo pulso (ponto claro) foi seguido de 2 quadros sem pulso.
    //    Antes da 1a medicao aceita qualquer um depois do pulso do gatilho; depois, so
    //    perto da ida medida (-2..+3 quadros): o pato passando sob a mira nao engana.
    if (ultimoPulsoUs != 0 && (int32_t)(agoraUs - ultimoPulsoUs) > F * 5 / 2) {
      uint32_t c = ultimoPulsoUs + QUADRO_US;
      int32_t rel = (int32_t)(c - tiro.tGat);
      bool perto = nIda == 0 || (rel >= (int32_t)idaUs - 2 * F && rel <= (int32_t)idaUs + 3 * F);
      if (rel > 3 * F && perto) {
        defineT0(c, SYNC_CEU);
        guardaIda((uint32_t)rel);
      }
    }
    // 2) sem sincronia ate passar a janela: usa gatilho + ida e confere o historico.
    //    So com ida ja medida: sem medicao nao ha estimativa confiavel (aprende no ceu).
    if (!tiro.temT0 && nIda > 0 && (int32_t)(agoraUs - tiro.tGat) >= (int32_t)idaUs + 5 * F) {
      defineT0(tiro.tGat + idaUs, SYNC_ESTIMADA);
    }
  }

  if (tiro.temT0 && tiro.sync != SYNC_FALHOU && tiro.decidiu == DEC_NADA) {
    int32_t d = (int32_t)(agoraUs - tiro.t0);
    // 3) janela 1 (quadros 2-3): primeira luz -> Start, na hora
    if (tiro.luzJ1 && d < 8 * F) mandaMensagem(DEC_ALVO1, agoraUs, agoraMs);
    // 4) janela 2 (quadros 5-6) com o quadro 7 escuro -> Select. No Game A a cena volta
    //    no quadro 5; mirando no ceu a luz continua no 7, e isso NAO e acerto.
    else if (d >= F * 15 / 2 && d < 8 * F && tiro.luzJ2 && !tiro.luzQ7) mandaMensagem(DEC_ALVO2, agoraUs, agoraMs);
  }

  // 5) fim do tiro (e do bloqueio do gatilho)
  uint32_t fim = tiro.temT0 ? tiro.t0 + 8 * QUADRO_US + BLOQUEIO_US : tiro.tGat + 600000;
  if ((int32_t)(agoraUs - fim) >= 0) {
    logTiro();
    tiro.ativo = false;
  }
}

// ---------------- aviso do sensor no monitor serial ----------------
// Avisa quando o sensor comeca e para de ver luz (= quando o A aperta e solta), com a leitura e
// os pulsos por segundo. So escreve na mudanca: a piscada preta do tiro (~33 ms) e curta demais
// para virar "escuro".
void avisaSensor(uint32_t agora, int leitura, bool temLuz) {
  static bool vendoLuz = false;
  static uint32_t luzDesde = 0, pulsosDesde = 0;
  if (temLuz == vendoLuz) return;
  vendoLuz = temLuz;
  if (vendoLuz) {
    luzDesde = agora;
    pulsosDesde = totalPulsos;
    Serial.printf("### SENSOR: LUZ (leitura %d, limiar %.0f)\n", leitura, linhaBase + DELTA_LUZ);
  } else {
    uint32_t durMs = ultimaLuz - luzDesde;
    uint32_t n = totalPulsos - pulsosDesde;
    if (durMs >= 200)
      Serial.printf("### SENSOR: escuro (viu luz por %lu ms: %lu pulsos, %lu por segundo)\n",
                    (unsigned long)durMs, (unsigned long)n, (unsigned long)(n * 1000UL / durMs));
    else
      Serial.printf("### SENSOR: escuro (viu luz por %lu ms)\n", (unsigned long)durMs);
  }
}

// ---------------- bateria ----------------
int nivelBateria() {
  if (PINO_BATERIA < 0) return 100;
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PINO_BATERIA);
  float v = (mv / 8.0f) / 1000.0f * 2.0f * BAT_CORRECAO;   // divisor 100k/100k
  float p = (v - BAT_VAZIA_V) / (BAT_CHEIA_V - BAT_VAZIA_V) * 100.0f;
  return constrain((int)p, 0, 100);
}

// ---------------- USB ligada no computador? ----------------
bool usbConectado() {
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
  return Serial.isConnected();       // true quando o PC esta "conversando" com a placa
#else
  return false;
#endif
}

// ---------------- reinicia direto no modo de gravacao (comando 'b') ----------------
void modoGravacao() {
  Serial.println("Entrando no modo de gravacao... pode enviar o programa pela IDE.");
  delay(100);
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
}

// ---------------- sono profundo ----------------
void dormir() {
  Serial.println("Dormindo... puxe o gatilho para acordar.");
  delay(50);
  rtc_gpio_pullup_en((gpio_num_t)PINO_GATILHO);
  rtc_gpio_pulldown_dis((gpio_num_t)PINO_GATILHO);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PINO_GATILHO, 0);   // acorda com o gatilho em LOW
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  pinMode(PINO_GATILHO, INPUT_PULLUP);
  if (PINO_START >= 0)  pinMode(PINO_START, INPUT_PULLUP);
  if (PINO_SELECT >= 0) pinMode(PINO_SELECT, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(PINO_SENSOR, ADC_11db);
  if (PINO_BATERIA >= 0) analogSetPinAttenuation(PINO_BATERIA, ADC_11db);

#if USAR_BLUETOOTH
  // Anuncia um controle "completo" (16 botoes, direcional e 2 eixos) para o
  // Recalbox/Linux reconhecer como gamepad. Na pratica usa os botoes 1, 2, 11 e 12 (e o 13 no
  // modo mapeamento); eixos e direcional ficam sempre centralizados.
  BleGamepadConfiguration cfg;
  cfg.setControllerType(CONTROLLER_TYPE_GAMEPAD);
  cfg.setButtonCount(16);
  cfg.setHatSwitchCount(1);
  cfg.setWhichAxes(true, true, false, false, false, false, false, false);   // X e Y
  cfg.setAxesMin(-32767);
  cfg.setAxesMax(32767);                 // 0 = centro
  cfg.setAutoReport(false);              // so envia quando algo muda (menos atraso/consumo)
  gamepad.begin(&cfg);
#else
  gamepad.begin();
  USB.begin();
#endif

  // gatilho segurado por 3 s ao ligar -> modo mapeamento
  if (digitalRead(PINO_GATILHO) == LOW) {
    uint32_t t0 = millis();
    while (digitalRead(PINO_GATILHO) == LOW && millis() - t0 < 3000) delay(10);
    if (millis() - t0 >= 3000) {
      modoMapa = true;
      Serial.println(MSG_MAPA);
      while (digitalRead(PINO_GATILHO) == LOW) delay(10);   // espera soltar
    }
  }

  // linha de base inicial: media de leituras com a pistola parada
  long soma = 0;
  for (int i = 0; i < 200; i++) { soma += analogRead(PINO_SENSOR); delayMicroseconds(200); }
  linhaBase = soma / 200.0f;
  ultimaAtividade = millis();
  Serial.printf("Revolver do Duck Hunt pronto. Modo: %s | ROM V5: a pistola decide o acerto\n",
                modoMapa ? "MAPEAMENTO" : "normal");
  Serial.println("Aponte o sensor para a TV ou uma lampada: aparece '### SENSOR: LUZ' e o A aperta. 'd' mostra a leitura a cada 100 ms.");
  Serial.println("Para configurar no Recalbox: 'm' liga o modo mapeamento (para mapear Start, Select e Hotkey).");
}

void loop() {
  uint32_t agora = millis();
  uint32_t agoraUs = micros();

  // ---------- sensor de luz ----------
  int leitura = analogRead(PINO_SENSOR);
  if (leitura > leituraMax) leituraMax = leitura;
  bool pulso = leitura > linhaBase + DELTA_LUZ;
  static uint32_t ultimoBase = 0;
  static bool acima = false;
  if (pulso) {
    ultimaLuz = agora;
    // borda de subida de um pulso do tubo (1 por quadro): guarda o instante
    if (!acima && (ultimoPulsoUs == 0 || agoraUs - ultimoPulsoUs > SURDEZ_US)) {
      guardaPulso(agoraUs);
      if (tiro.ativo && tiro.temT0) classificaPulso(agoraUs);
    }
  } else if (agora != ultimoBase) {
    // acompanha devagar (~0,5 s) a luz ambiente / brilho medio da tela, 1x por ms,
    // para nao "engolir" o flash do quadrado branco
    ultimoBase = agora;
    linhaBase += (leitura - linhaBase) * 0.002f;
  }
  acima = pulso;

  // ---------- gatilho (com filtro) ----------
  static bool gatilhoEstavel = false;
  static uint32_t pulsoAte = 0, puxouEm = 0, ultimaPuxada = 0;
  static uint8_t puxadas = 0;           // modo mapeamento: puxadas curtas seguidas
  bool g = digitalRead(PINO_GATILHO) == LOW;
  if (g != gatilhoBruto) { gatilhoBruto = g; mudouGatilho = agora; }
  if (agora - mudouGatilho >= DEBOUNCE_MS && gatilhoBruto != gatilhoEstavel) {
    gatilhoEstavel = gatilhoBruto;
    ultimaAtividade = agora;
    if (modoMapa) {
      if (gatilhoEstavel) puxouEm = agora;
      else if (agora - puxouEm > 1000) { puxadas = 0; pulsoBotao(BOTAO_B); }   // puxada longa = B
      else { puxadas++; ultimaPuxada = agora; }                               // curta: conta
    } else if (gatilhoEstavel) {          // acabou de puxar: gera um pulso curto em B
      if (tiro.ativo) {
        Serial.println("(gatilho ignorado: tiro anterior ainda em andamento)");
      } else {
        pulsoAte = agora + PULSO_B_MS;
        comecaTiro(agoraUs);
      }
    }
  }
  // o jogo dispara quando o bit do gatilho volta a 0, como na Zapper real:
  // por isso B fica apertado so durante o pulso, mesmo segurando o gatilho
  bool gatilho = (int32_t)(pulsoAte - agora) > 0;

  // modo mapeamento: 0,4 s depois da ultima puxada curta, manda Start, Select ou Home
  if (modoMapa && puxadas && !gatilhoEstavel && agora - ultimaPuxada > JANELA_PUXADAS_MS) {
    static const uint8_t porPuxadas[] = { BOTAO_START, BOTAO_SELECT, BOTAO_HOTKEY };
    pulsoBotao(porPuxadas[min(puxadas, (uint8_t)3) - 1]);
    puxadas = 0;
  }

  // ---------- o que mandar ----------
  cuidaDoTiro(micros(), agora);
  bool luz = (agora - ultimaLuz) < SEGURA_LUZ_MS && ultimaLuz != 0;   // luz, "segurada" entre pulsos
  // botoes fisicos: bloqueados durante um tiro (do gatilho ao fim do bloqueio), porque nos ~150 ms
  // depois da piscada o jogo leria o Start / Select como "acertou"
  bool startFisico  = leBotao(fisStart, agora)  && !tiro.ativo;
  bool selectFisico = leBotao(fisSelect, agora) && !tiro.ativo;
  uint32_t m = 0;
  if (luz)                                                   m |= mascara(BOTAO_A);
  if (gatilho)                                                m |= mascara(BOTAO_B);
  if (startFisico  || (int32_t)(msgStartAte - agora) > 0)     m |= mascara(BOTAO_START);
  if (selectFisico || (int32_t)(msgSelectAte - agora) > 0)    m |= mascara(BOTAO_SELECT);
  // o Select pode ser o Hotkey no Recalbox: nunca manda o A (luz, que aperta sozinho) junto com
  // ele, senao Hotkey + A viraria um atalho do Recalbox no meio do jogo
  if (m & mascara(BOTAO_SELECT)) m &= ~mascara(BOTAO_A);

  // ---------- envia so quando muda ----------
  if (m != saida) {
    saida = m;
    enviar(saida);
  }

  avisaSensor(agora, leitura, luz);   // depois do envio: a mensagem nunca atrasa o botao

  // ---------- avisa quando o Bluetooth conecta/desconecta ----------
#if USAR_BLUETOOTH
  // 0 = sem conexao | 1 = conexao aberta, mas o host NAO completou o pareamento seguro
  // 2 = conectado como controle (pareado e criptografado: os botoes chegam)
  static int estadoPrev = -1;
  static uint32_t entrouEstado = 0, ultimoPedido = 0;
  static int pedidos = 0;
  static bool intervaloPedido = false, intervaloInformado = false;
  NimBLEServer* srv = NimBLEDevice::getServer();

  // Seguranca: pede pareamento COM chaves guardadas (bonding), sem PIN, com Secure Connections.
  // Sem isso o Linux/Recalbox pareia mas nao cria o controle (fica "Bonded: no").
  static bool segurancaOk = false;
  if (!segurancaOk && srv) {
    NimBLEDevice::setSecurityAuth(true, false, true);          // bonding, sem MITM, Secure Connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);  // "Just Works" (sem PIN)
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    segurancaOk = true;
    Serial.println("### Seguranca: pareamento com chaves guardadas (bonding) ativado");
  }
  int links = srv ? srv->getConnectedCount() : 0;
  int estado = gamepad.isConnected() ? 2 : (links > 0 ? 1 : 0);
  if (estado != estadoPrev) {
    estadoPrev = estado;
    entrouEstado = agora;
    pedidos = 0;
    intervaloPedido = intervaloInformado = false;
    if (estado == 2) {
      bool bonded = srv && links > 0 && srv->getPeerInfo(0).isBonded();
      Serial.printf("### BLUETOOTH: CONECTADO como controle | chaves guardadas (bonded): %s\n",
                    bonded ? "SIM" : "NAO - o Recalbox nao vai criar o controle");
    }
    else if (estado == 1) Serial.println("### BLUETOOTH: conexao aberta, MAS SEM PAREAMENTO SEGURO (o host nao terminou o pareamento)");
    else                  Serial.println("### BLUETOOTH: desconectado (aguardando o Recalbox)");
    if (estado == 2) enviar(saida);   // manda o estado atual ao conectar
  }
  // conectado sem pareamento seguro: a PISTOLA pede o pareamento ao Recalbox
  // (1 s depois de conectar e de novo a cada 5 s, ate 5 tentativas)
  if (estado == 1 && srv && pedidos < 5 && agora - entrouEstado > 1000 &&
      (pedidos == 0 || agora - ultimoPedido > 5000)) {
    ultimoPedido = agora;
    pedidos++;
    NimBLEConnInfo peer = srv->getPeerInfo(0);
    NimBLEDevice::startSecurity(peer.getConnHandle());
    Serial.printf("### Pedindo pareamento seguro ao Recalbox (tentativa %d de 5)...\n", pedidos);
  }
  // conectado como controle: 1 s depois pede o intervalo curto; 3 s depois mostra o
  // intervalo que o Recalbox aceitou (o Linux guarda e usa nas proximas conexoes)
  if (estado == 2 && srv && links > 0) {
    if (!intervaloPedido && agora - entrouEstado > 1000) {
      intervaloPedido = true;
      NimBLEConnInfo peer = srv->getPeerInfo(0);
      Serial.printf("### Intervalo de conexao atual: %.2f ms. Pedindo %.2f a %.2f ms...\n",
                    peer.getConnInterval() * 1.25f,
                    BLE_INTERVALO_MIN * 1.25f, BLE_INTERVALO_MAX * 1.25f);
      // latencia 0 = a pistola responde em todo encontro; timeout 200 x 10 ms = 2 s
      srv->updateConnParams(peer.getConnHandle(), BLE_INTERVALO_MIN, BLE_INTERVALO_MAX, 0, 200);
    }
    if (intervaloPedido && !intervaloInformado && agora - entrouEstado > 3000) {
      intervaloInformado = true;
      float ms = srv->getPeerInfo(0).getConnInterval() * 1.25f;
      Serial.printf("### Intervalo de conexao agora: %.2f ms %s\n", ms,
                    ms <= BLE_INTERVALO_MAX * 1.25f ? "(OK)" : "(o Recalbox NAO aceitou o pedido)");
    }
  }
#endif

  // ---------- bateria (a cada 30 s) ----------
#if USAR_BLUETOOTH
  if (agora - ultimaBateria > 30000) {
    ultimaBateria = agora;
    gamepad.setBatteryLevel(nivelBateria());
  }
#endif

  // ---------- comandos e debug pelo monitor serial ----------
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'd') debug = !debug;
    if (c == 'b') modoGravacao();
    if (c == 'm') {
      modoMapa = !modoMapa;
      if (modoMapa) Serial.println(MSG_MAPA);
      else          Serial.println("Modo normal (a pistola decide o acerto)");
    }
    if (c == '+') { DELTA_LUZ = max(20, DELTA_LUZ - 20); Serial.printf("DELTA_LUZ = %d (mais sensivel)\n", DELTA_LUZ); }
    if (c == '-') { DELTA_LUZ += 20; Serial.printf("DELTA_LUZ = %d (menos sensivel)\n", DELTA_LUZ); }
    if (c == 't') { modoTeste = !modoTeste;
                    Serial.printf("MODO TESTE %s (decide, mas %s Start / Select)\n", modoTeste ? "LIGADO" : "desligado",
                                  modoTeste ? "NAO manda" : "manda"); }
  }
  if (debug && agora - ultimoDebug > 100) {
    ultimoDebug = agora;
    Serial.printf("leitura max=%4d  base=%4.0f  limiar=%4.0f  botoes=%04lX  tiro=%d  ida=%.0f ms\n",
                  leituraMax, linhaBase, linhaBase + DELTA_LUZ, (unsigned long)saida, tiro.ativo, idaUs / 1000.0f);
    leituraMax = 0;
  }

  // ---------- economia de pilha ----------
  // nunca dorme ligada no computador (assim a IDE sempre encontra a porta)
  if (usbConectado()) ultimaAtividade = agora;
  if (agora - ultimaAtividade > DORMIR_APOS_MS) dormir();
}
