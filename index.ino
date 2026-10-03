#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <ESP8266WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <TZ.h>
#include <sntp.h>

// Altere somente as credenciais e, se desejar, os limites de retencao.
const char *SSID = "WIFI";
const char *SENHA = "SENHA";
const uint8_t CS_SD = D4;
const uint8_t BUZZER = D0;
const uint8_t INA226 = 0x40;
const float RESISTENCIA_SHUNT = 0.075f / 20.0f;
const uint32_t INTERVALO_MS = 1000UL;
// Limite de MEDICOES: o cabecalho nao entra nesta contagem.
const uint32_t MAX_LINHAS_ARQUIVO = 10000UL;
const uint16_t MAX_ARQUIVOS = 100;
static_assert(MAX_LINHAS_ARQUIVO > 0, "Limite de linhas deve ser maior que zero");
static_assert(MAX_ARQUIVOS > 0, "Retencao deve ser maior que zero");

Adafruit_SSD1306 tela(128, 64, &Wire, -1);
bool oledOK = false, sdOK = false, wifiAnterior = false;
bool horaAnunciada = false;
char nomeArquivo[13] = "";
uint32_t ultimaMedicao = 0;
uint32_t linhasArquivo = 0;

bool lerRegistro16(uint8_t registro, uint16_t &valor) {
  Wire.beginTransmission(INA226);
  Wire.write(registro);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(INA226, (uint8_t)2) != 2) return false;
  uint8_t alto = Wire.read();
  uint8_t baixo = Wire.read();
  valor = ((uint16_t)alto << 8) | baixo;
  return true;
}

bool obterHorario(char *data, size_t tamanho, char *curto, size_t tamCurto) {
  time_t agora = time(nullptr);
  if (agora < 1704067200) {
    snprintf(data, tamanho, "SEM_HORA");
    snprintf(curto, tamCurto, "NTP aguardando");
    return false;
  }
  struct tm local;
  localtime_r(&agora, &local);
  strftime(data, tamanho, "%Y-%m-%dT%H:%M:%S%z", &local);
  strftime(curto, tamCurto, "%d/%m %H:%M:%S", &local);
  return true;
}

void verificarWiFi() {
  bool conectado = WiFi.status() == WL_CONNECTED;
  if (conectado == wifiAnterior) return;
  wifiAnterior = conectado;
  if (conectado) {
    Serial.print("Wi-Fi conectado. IP: ");
    Serial.println(WiFi.localIP());
    sntp_servermode_dhcp(0);
    configTime(TZ_America_Sao_Paulo, "pool.ntp.org", "time.cloudflare.com");
    Serial.println("Sincronizacao NTP iniciada.");
  } else {
    Serial.println("Wi-Fi desconectado.");
  }
}

// Apenas L000000.CSV ... L999999.CSV na raiz pertencem a esta rotacao.
// M0000.CSV, teste.txt e quaisquer outros arquivos sao preservados.
bool numeroLog(const char *nome, uint32_t &numero) {
  String base(nome);
  int barra = base.lastIndexOf('/');
  if (barra >= 0) base = base.substring(barra + 1);
  base.toUpperCase();
  if (base.length() != 11 || base[0] != 'L' ||
      base.substring(7) != ".CSV") return false;
  numero = 0;
  for (uint8_t i = 1; i <= 6; ++i) {
    if (base[i] < '0' || base[i] > '9') return false;
    numero = numero * 10 + (base[i] - '0');
  }
  return true;
}

bool catalogar(uint32_t &quantidade, uint32_t &menor, uint32_t &maior) {
  quantidade = 0;
  menor = 1000000;
  maior = 0;
  File raiz = SD.open("/");
  if (!raiz || !raiz.isDirectory()) {
    if (raiz) raiz.close();
    return false;
  }
  while (true) {
    File item = raiz.openNextFile();
    if (!item) break;
    uint32_t numero;
    if (!item.isDirectory() && numeroLog(item.name(), numero)) {
      ++quantidade;
      if (numero < menor) menor = numero;
      if (numero > maior) maior = numero;
    }
    item.close();
    yield();
  }
  raiz.close();
  return true;
}

bool criarArquivo() {
  uint32_t quantidade, menor, maior;
  if (!catalogar(quantidade, menor, maior)) return false;
  // Ordem numerica permanece valida sem depender de NTP ou data FAT.
  uint32_t proximo = quantidade ? maior + 1 : 0;
  if (proximo > 999999) {
    Serial.println("Limite de numeracao dos logs atingido.");
    return false;
  }
  // Apaga ANTES de criar: nunca excede MAX_ARQUIVOS.
  while (quantidade >= MAX_ARQUIVOS) {
    char antigo[13];
    snprintf(antigo, sizeof(antigo), "/L%06lu.CSV", (unsigned long)menor);
    if (!SD.remove(antigo)) {
      Serial.print("Falha ao apagar: ");
      Serial.println(antigo);
      return false;
    }
    Serial.print("Retencao: removido ");
    Serial.println(antigo);
    if (!catalogar(quantidade, menor, maior)) return false;
  }
  char novo[13];
  snprintf(novo, sizeof(novo), "/L%06lu.CSV", (unsigned long)proximo);
  if (SD.exists(novo)) return false;
  File arquivo = SD.open(novo, FILE_WRITE);
  if (!arquivo) return false;
  const char cabecalho[] =
      "data_hora_local,tempo_ms,tensao_V,shunt_mV,corrente_A,potencia_W\n";
  size_t escritos = arquivo.write((const uint8_t *)cabecalho,
                                  sizeof(cabecalho) - 1);
  arquivo.close();
  if (escritos != sizeof(cabecalho) - 1) {
    SD.remove(novo);
    return false;
  }
  snprintf(nomeArquivo, sizeof(nomeArquivo), "%s", novo);
  linhasArquivo = 0;
  Serial.print("Novo CSV: ");
  Serial.println(nomeArquivo);
  return true;
}

bool gravarMedicao(const char *data, uint32_t tempo, float volts,
                  float milivolts, float corrente, float potencia) {
  char linha[160];
  int tamanho = snprintf(linha, sizeof(linha),
      "%s,%lu,%.3f,%.3f,%.6f,%.6f\n", data, (unsigned long)tempo,
      volts, milivolts, corrente, potencia);
  if (tamanho <= 0 || tamanho >= (int)sizeof(linha)) return false;
  // A medicao 10001 inicia o proximo CSV. Sem dependencia de hora ou Wi-Fi.
  if (linhasArquivo >= MAX_LINHAS_ARQUIVO) {
    if (!criarArquivo()) return false;
  }
  File arquivo = SD.open(nomeArquivo, FILE_WRITE);
  if (!arquivo) return false;
  size_t escritos = arquivo.write((const uint8_t *)linha, tamanho);
  arquivo.close(); // Fecha a cada amostra; nenhum arquivo aberto ao rotacionar.
  if (escritos != (size_t)tamanho) return false;
  ++linhasArquivo;
  return true;
}

// Quatro barras indicam o RSSI; os limiares sao uma escala visual aproximada.
void desenharWiFi() {
  bool conectado = WiFi.status() == WL_CONNECTED;
  // Icone de Wi-Fi, ao lado das barras.
  tela.drawLine(89, 3, 92, 1, SSD1306_WHITE);
  tela.drawLine(92, 1, 98, 1, SSD1306_WHITE);
  tela.drawLine(98, 1, 101, 3, SSD1306_WHITE);
  tela.drawLine(92, 5, 94, 4, SSD1306_WHITE);
  tela.drawLine(94, 4, 96, 4, SSD1306_WHITE);
  tela.drawLine(96, 4, 98, 5, SSD1306_WHITE);
  tela.fillRect(95, 7, 2, 2, SSD1306_WHITE);
  if (!conectado) {
    // Corte diagonal sobre o proprio icone, com margem escura para contraste.
    tela.drawLine(89, 9, 100, 0, SSD1306_BLACK);
    tela.drawLine(91, 9, 102, 0, SSD1306_BLACK);
    tela.drawLine(90, 9, 101, 0, SSD1306_WHITE);
    return;
  }
  int32_t rssi = WiFi.RSSI();
  uint8_t barras = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1;
  for (uint8_t n = 0; n < 4; ++n) {
    int16_t x = 106 + n * 5;
    int16_t altura = 2 + n * 2;
    if (n < barras) tela.fillRect(x, 9 - altura, 4, altura, SSD1306_WHITE);
    else tela.drawRect(x, 9 - altura, 4, altura, SSD1306_WHITE);
  }
}

void mostrarLogo() {
  if (!oledOK) return;
  tela.clearDisplay();
  tela.setTextWrap(false);
  tela.setTextColor(SSD1306_WHITE);
  // Logo desenhado diretamente no OLED: bateria com raio.
  tela.drawRoundRect(43, 1, 38, 24, 3, SSD1306_WHITE);
  tela.fillRect(81, 8, 4, 10, SSD1306_WHITE);
  tela.fillTriangle(63, 4, 54, 14, 63, 14, SSD1306_WHITE);
  tela.fillTriangle(59, 12, 68, 12, 59, 22, SSD1306_WHITE);
  tela.setTextSize(2);
  tela.setCursor(22, 30);
  tela.print("MONITOR");
  tela.setTextSize(1);
  tela.setCursor(34, 50);
  tela.print("DE ENERGIA");
  tela.display();
  delay(3000); // ESP8266 continua atendendo Wi-Fi durante delay().
}

void desenharMedicao(char unidade, float valor, uint8_t y, bool valida) {
  char texto[24];
  if (valida) snprintf(texto, sizeof(texto), "%.3f", valor);
  else snprintf(texto, sizeof(texto), "---");
  tela.setTextSize(2);
  tela.setCursor(4, y);
  tela.print(unidade);
  tela.print(':');
  // Fonte padrao: 6 pixels por caractere, 12 na escala 2.
  // Reduz apenas numeros excepcionalmente longos, evitando cortes.
  uint8_t escala = strlen(texto) * 12 <= 92 ? 2 : 1;
  tela.setTextSize(escala);
  int16_t largura = strlen(texto) * 6 * escala;
  tela.setCursor(124 - largura, y + (escala == 1 ? 4 : 0));
  tela.print(texto);
}

void atualizarTela(bool leituraOK, float v, float i, float p,
                   const char *horario) {
  if (!oledOK) return;
  tela.clearDisplay();
  tela.setTextWrap(false);
  tela.setTextColor(SSD1306_WHITE);
  tela.setTextSize(1);
  tela.setCursor(0, 0);
  // obterHorario() fornece "dd/mm HH:MM:SS" quando ha hora valida.
  if (horario[0] >= '0' && horario[0] <= '9') {
    for (uint8_t n = 0; n < 5; ++n) tela.write(horario[n]);
  } else {
    tela.print("--/--");
  }
  tela.setCursor(44, 0);
  tela.print(sdOK ? "SD:OK" : "SD:ERR");
  desenharWiFi();
  tela.drawFastHLine(4, 11, 120, SSD1306_WHITE);
  // A faixa azul comeca em y=16. Fonte padrao: 14 pixels visiveis
  // na escala 2, mais 2 pixels de espacamento entre as linhas.
  desenharMedicao('V', v, 16, leituraOK);
  desenharMedicao('A', i, 32, leituraOK);
  desenharMedicao('W', p, 48, leituraOK);
  tela.display();
}

void setup() {
  digitalWrite(BUZZER, HIGH);
  pinMode(BUZZER, OUTPUT);
  Serial.begin(115200);
  delay(500);
  Wire.begin(D2, D1);
  Wire.setClock(100000);
  oledOK = tela.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledOK) Serial.println("Falha ao inicializar OLED.");
  pinMode(CS_SD, OUTPUT);
  digitalWrite(CS_SD, HIGH);
  SPI.begin();
  sdOK = SD.begin(CS_SD);
  if (sdOK) sdOK = criarArquivo();
  if (!sdOK) Serial.println("Falha SD: gravacao desativada.");
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(SSID, SENHA);
  Serial.println("Aguardando Wi-Fi e horario NTP...");
  mostrarLogo();
  atualizarTela(false, 0, 0, 0, "NTP aguardando");
  ultimaMedicao = millis();
}

void loop() {
  verificarWiFi();
  uint32_t agora = millis();
  if ((uint32_t)(agora - ultimaMedicao) < INTERVALO_MS) {
    delay(1);
    return;
  }
  ultimaMedicao = agora;
  char data[32], horario[24];
  bool horaOK = obterHorario(data, sizeof(data), horario, sizeof(horario));
  if (horaOK && !horaAnunciada) {
    horaAnunciada = true;
    Serial.print("Hora sincronizada: "); Serial.println(data);
  }
  uint16_t brutoBarramento, brutoShunt;
  if (!lerRegistro16(0x02, brutoBarramento) ||
      !lerRegistro16(0x01, brutoShunt)) {
    Serial.println("Falha ao ler INA226.");
    atualizarTela(false, 0, 0, 0, horario);
    return;
  }
  float volts = brutoBarramento * 0.00125f;
  float voltsShunt = (int16_t)brutoShunt * 0.0000025f;
  float corrente = voltsShunt / RESISTENCIA_SHUNT;
  float potencia = volts * corrente;
  if (sdOK && !gravarMedicao(data, agora, volts, voltsShunt * 1000.0f,
                             corrente, potencia)) {
    sdOK = false;
    Serial.println("Falha SD: gravacao interrompida. Verifique o cartao.");
  }
  Serial.print(data);
  Serial.print(" | Tensao: "); Serial.print(volts, 3); Serial.print(" V");
  Serial.print(" | Corrente: "); Serial.print(corrente, 3); Serial.print(" A");
  Serial.print(" | Potencia: "); Serial.print(potencia, 3); Serial.print(" W");
  Serial.print(sdOK ? " | SD: OK" : " | SD: ERRO");
  Serial.println(WiFi.status() == WL_CONNECTED ? " | WiFi: OK"
                                              : " | WiFi: DESCONECTADO");
  atualizarTela(true, volts, corrente, potencia, horario);
}