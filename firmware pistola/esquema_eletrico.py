"""Gera o esquema elétrico da pistola (esquema_eletrico.svg) e, com o Edge em modo headless,
o PNG (esquema_eletrico.png, 3200x2000).

Uso:
    python esquema_eletrico.py            # SVG + PNG
    python esquema_eletrico.py --so-svg   # só o SVG

Para mudar o desenho, edite este arquivo (coordenadas em pixels do PNG final).
"""
import math
import subprocess
import sys
from pathlib import Path

AQUI = Path(__file__).resolve().parent
SVG = AQUI / "esquema_eletrico.svg"
PNG = AQUI / "esquema_eletrico.png"
EDGE = [Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"),
        Path(r"C:\Program Files\Microsoft\Edge\Application\msedge.exe")]

L, A = 3200, 2000
PRETO, CINZA, VERDE, LARANJA = "#222222", "#8a8a8a", "#2a9d6f", "#e09b00"
AMARELO, TEXTO2 = "#eab308", "#555555"
FONTE = "DejaVu Sans, Verdana, Segoe UI, Arial, sans-serif"

el = []


def linha(pts, cor=PRETO, w=5, tracejado=False):
    d = " ".join(f"{x},{y}" for x, y in pts)
    extra = ' stroke-dasharray="14 10"' if tracejado else ""
    el.append(f'<polyline points="{d}" fill="none" stroke="{cor}" stroke-width="{w}" '
              f'stroke-linecap="round" stroke-linejoin="round"{extra}/>')


def texto(x, y, s, tam=28, cor=PRETO, negrito=False, ancora="start"):
    peso = ' font-weight="bold"' if negrito else ""
    el.append(f'<text x="{x}" y="{y}" font-size="{tam}" fill="{cor}"{peso} text-anchor="{ancora}">{s}</text>')


def ponto(x, y, cor=PRETO, r=11):
    el.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{cor}"/>')


def terminal(x, y, cor=PRETO):
    el.append(f'<circle cx="{x}" cy="{y}" r="10" fill="white" stroke="{cor}" stroke-width="5"/>')


def resistor(x, y0, y1, cor=PRETO, a=27):
    passo = (y1 - y0) / 6
    pts = [(x, y0)]
    for i in range(6):
        pts.append((x + (a if i % 2 == 0 else -a), y0 + passo * (i + 0.5)))
    pts.append((x, y1))
    linha(pts, cor)


def seta(x0, y0, x1, y1, cor, comp=32, meia=14):
    """Linha de (x0,y0) até a ponta (x1,y1), com a ponta da seta cheia."""
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy)
    ux, uy = dx / n, dy / n
    bx, by = x1 - comp * ux, y1 - comp * uy
    linha([(x0, y0), (bx, by)], cor)
    p1 = (bx - meia * uy, by + meia * ux)
    p2 = (bx + meia * uy, by - meia * ux)
    el.append(f'<polygon points="{x1},{y1} {p1[0]:.1f},{p1[1]:.1f} {p2[0]:.1f},{p2[1]:.1f}" fill="{cor}"/>')


def chave_vertical(x, y_cima, y_baixo, cor=PRETO):
    """Botão/microswitch aberto: dois terminais na vertical e a alavanca saindo do de baixo."""
    terminal(x, y_cima, cor)
    terminal(x, y_baixo, cor)
    linha([(x - 4, y_baixo - 10), (x - 58, y_cima - 6)], cor)


# ---------------- título e legenda ----------------
texto(80, 105, "Revolver do Duck Hunt — esquema elétrico", 64, negrito=True)
texto(80, 163, "ESP32-S3 Super Mini · 3× AA · TEPT4400 · gatilho microswitch · botões Start/Select · "
      "Bluetooth (BLE)", 32, TEXTO2)
linha([(2190, 124), (2270, 124)])
texto(2290, 134, "obrigatório", 30, TEXTO2)
linha([(2510, 124), (2590, 124)], VERDE)
texto(2610, 134, "recomendado", 30, TEXTO2)
linha([(2850, 124), (2930, 124)], CINZA, tracejado=True)
texto(2950, 134, "opcional", 30, TEXTO2)

# ---------------- pilhas e interruptor ----------------
for y in (879, 998, 1120):
    linha([(298, y), (421, y)], w=5)
for y in (911, 1031, 1152):
    linha([(327, y), (393, y)], w=11)
linha([(360, 879), (360, 640), (539, 640)])
linha([(360, 1152), (360, 1640)])
texto(140, 982, "3× AA", 34, negrito=True, ancora="middle")
texto(140, 1026, "4,5 V", 30, TEXTO2, ancora="middle")
texto(140, 1064, "(NiMH: 4×)", 24, TEXTO2, ancora="middle")
texto(454, 877, "+", 40, "#d1495b", ancora="middle")
texto(454, 1200, "−", 40, ancora="middle")
terminal(549, 640)
terminal(689, 640)
linha([(557, 635), (678, 579)])
texto(619, 537, "Interruptor", 32, negrito=True, ancora="middle")

# ---------------- diodo ----------------
linha([(699, 640), (899, 640)])
linha([(899, 640), (939, 640)], VERDE)
el.append(f'<polygon points="939,598 939,682 1010,640" fill="{VERDE}"/>')
linha([(1013, 596), (1013, 684)], VERDE, w=8)
linha([(1013, 640), (1279, 640)])
texto(975, 562, "Diodo 1N5819", 32, VERDE, negrito=True, ancora="middle")
texto(975, 728, "protege ao ligar o USB", 24, VERDE, ancora="middle")

# ---------------- divisor da bateria (opcional) ----------------
linha([(799, 640), (799, 813)], CINZA, tracejado=True)
resistor(799, 813, 959, CINZA)
linha([(799, 959), (799, 1174)], CINZA, tracejado=True)
linha([(799, 1079), (1279, 1079)], CINZA, tracejado=True)
resistor(799, 1174, 1320, CINZA)
linha([(799, 1320), (799, 1636)], CINZA, tracejado=True)
ponto(799, 640, CINZA, 10)
ponto(799, 1079, CINZA, 10)
texto(850, 897, "100 kΩ", 28, TEXTO2)
texto(850, 1259, "100 kΩ", 28, TEXTO2)
texto(850, 1440, "medidor de bateria", 28, TEXTO2)
texto(850, 1476, "(opcional)", 28, TEXTO2)

# ---------------- ESP32-S3 ----------------
el.append('<rect x="1279" y="500" width="519" height="800" rx="18" fill="#1f2937" stroke="#111111" stroke-width="3"/>')
el.append('<rect x="1390" y="800" width="236" height="179" rx="8" fill="#334155" stroke="#64748b" stroke-width="2"/>')
texto(1508, 880, "ESP32-S3", 32, "white", negrito=True, ancora="middle")
texto(1508, 925, "Super Mini", 26, "#cbd5e1", ancora="middle")
PINOS = [(1538, 500), (1279, 640), (1798, 659), (1798, 939), (1279, 1079), (1279, 1199), (1380, 1300), (1700, 1300)]
el.append('<rect x="1488" y="1238" width="100" height="62" rx="6" fill="#9ca3af"/>')
texto(1538, 1352, "USB-C (só gravar)", 24, TEXTO2, ancora="middle")
for x, y, nome, ancora, dx, dy in [
        (1538, 500, "3V3", "middle", 0, 58), (1279, 640, "5V", "start", 26, 12),
        (1798, 659, "GPIO1", "end", -26, 12), (1798, 939, "GPIO2", "end", -26, 12),
        (1279, 1079, "GPIO3", "start", 26, 12), (1279, 1199, "GND", "start", 26, 12),
        (1380, 1300, "GPIO4", "middle", 0, -32), (1700, 1300, "GPIO5", "middle", 0, -32)]:
    texto(x + dx, y + dy, nome, 32, "white", negrito=True, ancora=ancora)

# ---------------- sensor TEPT4400 ----------------
linha([(1538, 500), (1538, 340), (2298, 340), (2298, 458)])
el.append(f'<circle cx="2278" cy="529" r="80" fill="none" stroke="{PRETO}" stroke-width="5"/>')
linha([(2246, 483), (2246, 575)], w=8)
linha([(2246, 508), (2298, 458)])
seta(2246, 550, 2300, 602, PRETO, comp=30, meia=13)
linha([(2298, 600), (2298, 813)])
texto(2322, 420, "C", 26, TEXTO2)
texto(2322, 638, "E", 26, TEXTO2)
seta(2457, 452, 2365, 502, LARANJA)
seta(2477, 512, 2385, 562, LARANJA)
texto(2500, 440, "luz da tela", 28, LARANJA)
texto(2410, 640, "TEPT4400", 36, negrito=True)
texto(2410, 682, "C (coletor) em cima, E (emissor) embaixo", 26, TEXTO2)
texto(2410, 718, "confira as pernas no datasheet", 26, TEXTO2)
linha([(1798, 659), (2298, 659)])
texto(2040, 642, "sinal analógico", 26, TEXTO2, ancora="middle")
resistor(2298, 813, 978)
linha([(2298, 978), (2298, 1640)])
texto(2345, 882, "10 kΩ", 34, negrito=True)
texto(2345, 920, "(testar 4,7k–22k)", 26, TEXTO2)

# ---------------- gatilho (GPIO2) ----------------
linha([(1798, 939), (2079, 939), (2079, 1121)])
chave_vertical(2079, 1131, 1269)
linha([(2079, 1279), (2079, 1640)])
texto(1918, 920, "pull-up interno", 26, TEXTO2, ancora="middle")
texto(1998, 1180, "Gatilho", 34, negrito=True, ancora="end")
texto(1998, 1220, "microswitch", 26, TEXTO2, ancora="end")
texto(1998, 1254, "COM + NA", 26, TEXTO2, ancora="end")

# ---------------- botões Start (GPIO4) e Select (GPIO5) ----------------
for x, nome in ((1380, "Start"), (1700, "Select")):
    linha([(x, 1300), (x, 1410)], VERDE)
    chave_vertical(x, 1420, 1540, VERDE)
    linha([(x, 1550), (x, 1640)], VERDE)
    texto(x - 75, 1478, nome, 32, VERDE, negrito=True, ancora="end")
    texto(x - 75, 1512, "botão", 24, TEXTO2, ancora="end")

# ---------------- GND ----------------
linha([(1279, 1199), (1160, 1199), (1160, 1640)])
linha([(360, 1640), (2618, 1640), (2618, 1680)])
for x in (1160, 1380, 1700, 2079, 2298):
    ponto(x, 1640)
ponto(2298, 659)                      # emissor + GPIO1 + 10k
for x, y in PINOS:                    # pinos da placa, por cima dos fios
    ponto(x, y, AMARELO, 9)
for y, meia in ((1680, 40), (1700, 25), (1720, 10)):
    linha([(2618 - meia, y), (2618 + meia, y)])
texto(2680, 1652, "GND", 34, negrito=True)

# ---------------- notas ----------------
el.append('<rect x="80" y="1752" width="3040" height="226" rx="16" fill="#f6f7fa" stroke="#c9d0dc" stroke-width="3"/>')
notas_esq = [
    ("Pilhas no pino 5V", ", nunca no 3V3 (o regulador da placa baixa para 3,3 V)."),
    ("Sensor em pino ADC1", " (GPIO1–10 no S3). O ADC2 conflita com o rádio Bluetooth/Wi-Fi."),
    ("Gatilho, Start e Select:", " INPUT_PULLUP (GPIO2, GPIO4, GPIO5), apertado = LOW."),
    ("Start e Select:", " botões opcionais; durante um tiro o firmware os ignora."),
]
notas_dir = [
    ("USB-C:", " só para gravar. Por segurança, desligue o interruptor antes de conectar."),
    ("Pinos são sugestão:", " confira a serigrafia da sua placa."),
    ("Divisor 100k/100k:", " GPIO3 lê metade da tensão das pilhas."),
    ("Gamepad:", " A = luz, B = gatilho, Start = pato 1 ou botão, Select = pato 2 ou botão."),
]
for col, notas in ((120, notas_esq), (1700, notas_dir)):
    for i, (neg, resto) in enumerate(notas):
        y = 1806 + 48 * i
        el.append(f'<text x="{col}" y="{y}" font-size="27" fill="#333333">'
                  f'<tspan font-weight="bold">{neg}</tspan>{resto}</text>')

svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{L}" height="{A}" viewBox="0 0 {L} {A}" '
       f'font-family="{FONTE}">\n<rect width="{L}" height="{A}" fill="white"/>\n' + "\n".join(el) + "\n</svg>\n")
SVG.write_text(svg, encoding="utf-8")
print(f"SVG: {SVG}")

if "--so-svg" not in sys.argv:
    edge = next((e for e in EDGE if e.exists()), None)
    if edge is None:
        sys.exit("Edge não encontrado: abra o SVG no navegador e salve como PNG.")
    import tempfile
    with tempfile.TemporaryDirectory() as perfil:     # perfil separado: não se junta ao Edge aberto
        subprocess.run([str(edge), "--headless", "--disable-gpu", "--no-sandbox", "--hide-scrollbars",
                        "--force-device-scale-factor=1", f"--window-size={L},{A}",
                        f"--user-data-dir={perfil}", f"--screenshot={PNG}", SVG.as_uri()],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)
    if not PNG.exists():
        sys.exit("O Edge não gerou o PNG: abra o SVG no navegador e salve como PNG.")
    print(f"PNG: {PNG}")
