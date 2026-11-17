/*
  Teste da pistola: gatilho e sensor de luz
  Placa: ESP32-S3 Super Mini  (Arduino IDE: "ESP32S3 Dev Module", Flash Size "4MB",
         USB CDC On Boot "Enabled")

  Ligacoes (as mesmas do firmware da pistola):
    GPIO1 (ADC1) <- emissor do TEPT4400 + resistor 10k para o GND (coletor no 3,3 V)
    GPIO2        <- microswitch do gatilho (outro terminal no GND)

  Monitor Serial em 115200. A cada 200 ms aparece uma linha:
    agora     leitura atual do sensor (0-4095) e em mV
    min / max menor e maior leitura nos ultimos 200 ms
    base      nivel "normal" acompanhado devagar, igual ao firmware
    limiar    base + DELTA: acima disso o firmware conta um pulso de luz
    pulsos/s  pulsos por segundo (tela branca na TV de tubo: perto de 60)
    barra     o max dos ultimos 200 ms
    gatilho   solto / APERTADO e quantos tiros desde que ligou

  Comandos (digite no Monitor Serial e aperte Enter):
    f   teste do fio: liga o pull-up interno do GPIO1 por um instante e diz se o
        fio do sensor e o resistor de 10k estao ligados (tampe o sensor antes)
    p   liga/desliga o modo grafico (Ferramentas > Plotter Serial)
    +   mais sensivel (DELTA - 20)      -   menos sensivel (DELTA + 20)
    r   zera o contador de tiros
*/

#include <Arduino.h>
#include "driver/rtc_io.h"

const int PINO_SENSOR  = 1;
const int PINO_GATILHO = 2;

int DELTA_LUZ = 300;                 // o mesmo valor inicial do firmware
const uint32_t DEBOUNCE_MS = 5;      // filtro do microswitch
const uint32_t SURDEZ_US   = 3000;   // depois de um pulso, ignora ~3 ms (1 pulso por quadro)
const uint32_t LINHA_MS    = 200;    // uma linha no Monitor Serial a cada 200 ms
const uint32_t PLOT_MS     = 10;     // um ponto no Plotter Serial a cada 10 ms

// min/max da linha atual e do ponto atual do grafico
int lMin = 4095, lMax = 0;
int pMin = 4095, pMax = 0;
// min/max dos ultimos 2 s, para o aviso automatico
int dMin = 4095, dMax = 0;
uint32_t dInicio = 0;
const char* aviso = "";

// deteccao de pulsos, igual ao firmware
float    base = 0;
bool     acima = false;
uint32_t ultimoPulsoUs = 0, ultimaBase = 0;
uint32_t pulsosSeg = 0, pulsosContando = 0, inicioSeg = 0;

// gatilho
bool     gatBruto = false, gatEstavel = false;
uint32_t gatMudou = 0, gatApertouEm = 0, tiros = 0;

bool     plotter = false;
uint32_t ultimaLinha = 0, ultimoPlot = 0;

int mediaLeitura(int n) {
  long soma = 0;
  for (int i = 0; i < n; i++) { soma += analogRead(PINO_SENSOR); delayMicroseconds(100); }
  return soma / n;
}

// Com o sensor no escuro, o TEPT4400 quase nao conduz e o GPIO1 so "ve" o resistor de 10k.
// Ligando o pull-up interno (~45k para o 3,3 V), a tensao vai para:
//   ~0,6 V      -> o 10k esta la (fio e resistor OK)
//   ~3,3 V      -> nada segura o pino em baixo: GPIO1 solto (fio, resistor ou GND partido)
//   ~0 V        -> o GPIO1 esta em curto com o GND
void testeDoFio() {
  Serial.println("\n=== TESTE DO FIO (o sensor precisa estar TAMPADO, no escuro) ===");
  delay(300);
  int sem = mediaLeitura(100);
  rtc_gpio_pullup_en((gpio_num_t)PINO_SENSOR);
  delay(20);
  int com = mediaLeitura(100);
  rtc_gpio_pullup_dis((gpio_num_t)PINO_SENSOR);
  delay(20);
  Serial.printf("leitura sem pull-up: %4d | com pull-up: %4d\n", sem, com);
  if (com > 3500) {
    Serial.println("-> GPIO1 SOLTO: o resistor de 10k para o GND nao esta ligado ao pino.");
    Serial.println("   Confira o fio do GPIO1 ate o emissor/resistor, o fio do GND e as soldas.");
  } else if (com < 150) {
    Serial.println("-> GPIO1 em CURTO com o GND (ou o pull-up interno nao ligou).");
    Serial.println("   Confira se o emissor ou o fio do GPIO1 nao estao encostando direto no GND.");
  } else {
    Serial.println("-> Fio do GPIO1 e resistor de 10k OK.");
    Serial.println("   Se o sensor nao reage a luz, o problema esta no TEPT4400: sem 3,3 V no coletor,");
    Serial.println("   ligado invertido (coletor e emissor trocados) ou queimado.");
  }
  if (sem > 300)
    Serial.println("   (a leitura sem pull-up nao estava perto de 0: o sensor estava mesmo tampado?)");
  Serial.println("=================================================================\n");
  base = mediaLeitura(50);
  lMin = pMin = dMin = 4095;
  lMax = pMax = dMax = 0;
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);   // espera o Monitor Serial (USB) por ate 3 s

  pinMode(PINO_GATILHO, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(PINO_SENSOR, ADC_11db);

  base = mediaLeitura(200);
  gatBruto = gatEstavel = digitalRead(PINO_GATILHO) == LOW;
  dInicio = inicioSeg = millis();

  Serial.println("\n=== TESTE DA PISTOLA: gatilho (GPIO2) e sensor de luz (GPIO1) ===");
  Serial.println("1. Tampe o sensor com o dedo: 'agora' deve ficar perto de 0.");
  Serial.println("2. Aponte para uma lampada ou para a TV com tela branca: 'agora' e 'max' devem subir muito.");
  Serial.println("3. Na TV de tubo com tela branca, 'pulsos/s' deve ficar perto de 60.");
  Serial.println("4. Puxe o gatilho: aparece '### GATILHO'.");
  Serial.println("Comandos: f = teste do fio | p = grafico | + / - = sensibilidade | r = zera tiros\n");
  if (gatEstavel) {
    Serial.println("ATENCAO: o gatilho ja comecou APERTADO. Se voce nao esta segurando, o GPIO2 esta em");
    Serial.println("curto com o GND ou o microswitch esta ligado no terminal NC em vez do NO.\n");
  }
}

void loop() {
  uint32_t agora = millis();
  uint32_t agoraUs = micros();

  // ---------- sensor ----------
  int v = analogRead(PINO_SENSOR);
  if (v < lMin) lMin = v;
  if (v > lMax) lMax = v;
  if (v < pMin) pMin = v;
  if (v > pMax) pMax = v;
  if (v < dMin) dMin = v;
  if (v > dMax) dMax = v;

  bool pulso = v > base + DELTA_LUZ;
  if (pulso) {
    if (!acima && agoraUs - ultimoPulsoUs > SURDEZ_US) { ultimoPulsoUs = agoraUs; pulsosContando++; }
  } else if (agora != ultimaBase) {
    ultimaBase = agora;
    base += (v - base) * 0.002f;
  }
  acima = pulso;
  if (agora - inicioSeg >= 1000) { inicioSeg = agora; pulsosSeg = pulsosContando; pulsosContando = 0; }

  // aviso automatico, olhando os ultimos 2 s
  if (agora - dInicio >= 2000) {
    if (dMax < 30)         aviso = "SEMPRE ~0: tampe e destampe o sensor; se nada mudar, digite f";
    else if (dMin > 4050)  aviso = "SEMPRE NO MAXIMO: luz forte demais ou GPIO1 ligado no 3,3 V";
    else                   aviso = "";
    dMin = 4095; dMax = 0; dInicio = agora;
  }

  // ---------- gatilho ----------
  bool g = digitalRead(PINO_GATILHO) == LOW;
  if (g != gatBruto) { gatBruto = g; gatMudou = agora; }
  if (agora - gatMudou >= DEBOUNCE_MS && gatBruto != gatEstavel) {
    gatEstavel = gatBruto;
    if (gatEstavel) {
      tiros++;
      gatApertouEm = agora;
      if (!plotter) Serial.printf("### GATILHO APERTADO (tiro %lu)\n", (unsigned long)tiros);
    } else if (!plotter) {
      Serial.printf("### GATILHO SOLTO (ficou apertado %lu ms)\n", (unsigned long)(agora - gatApertouEm));
    }
  }

  // ---------- saida ----------
  if (plotter) {
    if (agora - ultimoPlot >= PLOT_MS) {
      ultimoPlot = agora;
      Serial.printf("max:%d,min:%d,limiar:%.0f,gatilho:%d\n", pMax, pMin, base + DELTA_LUZ, gatEstavel ? 4095 : 0);
      pMin = 4095; pMax = 0;
    }
  } else if (agora - ultimaLinha >= LINHA_MS) {
    ultimaLinha = agora;
    char barra[31];
    int n = lMax * 30 / 4096;
    for (int i = 0; i < 30; i++) barra[i] = i < n ? '#' : '.';
    barra[30] = 0;
    Serial.printf("agora %4d (%4d mV) | min %4d max %4d | base %4.0f limiar %4.0f | pulsos/s %3lu | %s | gatilho %s (%lu tiros)%s%s\n",
                  v, analogReadMilliVolts(PINO_SENSOR), lMin, lMax, base, base + DELTA_LUZ,
                  (unsigned long)pulsosSeg, barra, gatEstavel ? "APERTADO" : "solto   ",
                  (unsigned long)tiros, *aviso ? " | " : "", aviso);
    lMin = 4095; lMax = 0;
  }

  // ---------- comandos ----------
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'f') testeDoFio();
    else if (c == 'p') { plotter = !plotter; Serial.println(plotter ? "modo grafico LIGADO (abra o Plotter Serial)" : "modo grafico desligado"); }
    else if (c == '+') { DELTA_LUZ = max(20, DELTA_LUZ - 20); Serial.printf("DELTA_LUZ = %d (mais sensivel)\n", DELTA_LUZ); }
    else if (c == '-') { DELTA_LUZ += 20; Serial.printf("DELTA_LUZ = %d (menos sensivel)\n", DELTA_LUZ); }
    else if (c == 'r') { tiros = 0; Serial.println("contador de tiros zerado"); }
  }
}
