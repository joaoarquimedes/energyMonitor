#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <ESP8266WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <TZ.h>
#include <sntp.h>
#include <math.h>

// Altere somente as credenciais e, se desejar, os limites de retencao.
const char *SSID = "Cronin 2G";
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

// Ajustes sonoros escolhidos nos testes (percentual ativo, nao dB).
const uint16_t FREQUENCIA_INICIO_HZ = 3500;
const uint8_t VOLUME_INICIO = 12;
const uint16_t FREQUENCIA_ALARME_HZ = 3000;
const uint8_t VOLUME_ALARME = 20;
const uint32_t REPETICAO_ALARME_MS = 15000UL;
const uint32_t CONFIRMACAO_CONDICAO_MS = 3000UL;

// 0 desativa o respectivo limite. Preencher apos definir os limites da bateria/carga.
constexpr float TENSAO_BAIXA_V = 0.0f;
constexpr float TENSAO_CRITICA_V = 0.0f;
constexpr float CORRENTE_MAXIMA_A = 0.0f;
const float HISTERESE_TENSAO_V = 0.20f;
const float HISTERESE_CORRENTE_A = 0.05f;

enum TipoAlarme {
  SEM_ALARME, BATERIA_BAIXA, BATERIA_CRITICA,
  ERRO_SD, SOBRECORRENTE, FALHA_INA226
};
// Para testar tela e som sem provocar uma falha, escolha um tipo acima.
// Voltar a SEM_ALARME para funcionamento normal.
const TipoAlarme ALARME_TESTE = SEM_ALARME;
TipoAlarme alarmeAtual = SEM_ALARME, candidatoAlarme = SEM_ALARME;
uint32_t inicioCandidato = 0, ultimoSomAlarme = 0;
bool somAlarmePendente = false;
bool cartaoInicializado = false;
bool logAlertasOK = true;
bool historicoAlertas = false;
const char *ARQUIVO_ALERTAS = "/ALERTAS.CSV";
const char *ARQUIVO_ALERTAS_ANTERIOR = "/ALERTOLD.CSV";
const uint32_t MAX_BYTES_ALERTAS = 1024UL * 1024UL;
const uint32_t JANELA_LEITURA_ALERTAS = 8192UL;
// Prototipo explicito porque o registrador usa obterHorario(), definido adiante.
bool registrarTransicaoAlarme(TipoAlarme anterior, TipoAlarme novo,
                              bool leituraOK, float v, float i);
static_assert(VOLUME_INICIO <= 50 && VOLUME_ALARME <= 50,
              "Intensidade deve estar entre 0 e 50");
static_assert(TENSAO_BAIXA_V == 0 || TENSAO_CRITICA_V == 0 ||
              TENSAO_CRITICA_V < TENSAO_BAIXA_V,
              "Tensao critica deve ser menor que tensao baixa");

void silenciarBuzzer() { digitalWrite(BUZZER, HIGH); }

// Geracao manual, como no teste que funcionou com este buzzer no D0.
// Rotinas sonoras sao bloqueantes; durante o som o intervalo de amostragem
// pode aumentar. O CSV registra o tempo real em tempo_ms.
void beep(uint16_t frequencia, uint8_t intensidade, uint16_t duracaoMs) {
  if (intensidade == 0) { delay(duracaoMs); return; }
  uint32_t periodo = 1000000UL / frequencia;
  uint32_t ativo = periodo * intensidade / 100;
  uint32_t inicio = millis(), ultimoYield = inicio;
  while ((uint32_t)(millis() - inicio) < duracaoMs) {
    digitalWrite(BUZZER, LOW);
    delayMicroseconds(ativo);
    digitalWrite(BUZZER, HIGH);
    delayMicroseconds(periodo - ativo);
    if ((uint32_t)(millis() - ultimoYield) >= 10) {
      yield();
      ultimoYield = millis();
    }
  }
  silenciarBuzzer();
}

void beepInicializacao() { beep(FREQUENCIA_INICIO_HZ, VOLUME_INICIO, 80); }
void beepAlarme(uint16_t duracao) {
  beep(FREQUENCIA_ALARME_HZ, VOLUME_ALARME, duracao);
}
const char *mensagemAlarme(TipoAlarme tipo) {
  switch (tipo) {
    case BATERIA_BAIXA: return "BATERIA BAIXA";
    case BATERIA_CRITICA: return "BATERIA CRITICA";
    case ERRO_SD: return "FALHA NO CARTAO SD";
    case SOBRECORRENTE: return "SOBRECORRENTE";
    case FALHA_INA226: return "FALHA INA226";
    default: return "";
  }
}
void tocarPadraoAlarme(TipoAlarme tipo) {
  switch (tipo) {
    case BATERIA_BAIXA:
      beepAlarme(150); delay(150); beepAlarme(150); break;
    case BATERIA_CRITICA:
      beepAlarme(80); delay(80); beepAlarme(80); delay(80); beepAlarme(80); break;
    case ERRO_SD:
      beepAlarme(500); delay(180); beepAlarme(100); break;
    case SOBRECORRENTE:
      beepAlarme(700); delay(700); beepAlarme(700); break;
    case FALHA_INA226:
      for (uint8_t n = 0; n < 4; ++n) {
        beepAlarme(80);
        if (n < 3) delay(80);
      }
      break;
    default: break;
  }
}

TipoAlarme detectarAlarme(bool leituraOK, float v, float i) {
  if (ALARME_TESTE != SEM_ALARME) return ALARME_TESTE;
  // Prioridade: sensor, sobrecorrente, critica, SD, baixa.
  if (!leituraOK) return FALHA_INA226;
  float margemI = alarmeAtual == SOBRECORRENTE ? HISTERESE_CORRENTE_A : 0;
  if (CORRENTE_MAXIMA_A > 0 && fabsf(i) >= CORRENTE_MAXIMA_A - margemI)
    return SOBRECORRENTE;
  float margemCritica = alarmeAtual == BATERIA_CRITICA ? HISTERESE_TENSAO_V : 0;
  if (TENSAO_CRITICA_V > 0 && v <= TENSAO_CRITICA_V + margemCritica)
    return BATERIA_CRITICA;
  if (!sdOK || !logAlertasOK) return ERRO_SD;
  float margemBaixa = alarmeAtual == BATERIA_BAIXA ? HISTERESE_TENSAO_V : 0;
  if (TENSAO_BAIXA_V > 0 && v <= TENSAO_BAIXA_V + margemBaixa)
    return BATERIA_BAIXA;
  return SEM_ALARME;
}

void avaliarAlarme(bool leituraOK, float v, float i) {
  TipoAlarme novo = detectarAlarme(leituraOK, v, i);
  uint32_t agora = millis();
  if (novo != candidatoAlarme) {
    candidatoAlarme = novo;
    inicioCandidato = agora;
  }
  bool imediato = novo == FALHA_INA226 || novo == ERRO_SD ||
                  ALARME_TESTE != SEM_ALARME;
  if (novo != alarmeAtual &&
      (imediato || (uint32_t)(agora - inicioCandidato) >= CONFIRMACAO_CONDICAO_MS)) {
    TipoAlarme anterior = alarmeAtual;
    alarmeAtual = novo;
    if (!registrarTransicaoAlarme(anterior, novo, leituraOK, v, i)) {
      Serial.println("Falha ao registrar transicao de alarme no SD.");
    }
    somAlarmePendente = novo != SEM_ALARME;
    Serial.print("Alarme: ");
    Serial.println(novo == SEM_ALARME ? "NORMAL" : mensagemAlarme(novo));
  }
}

// Chamar DEPOIS de atualizarTela(): o aviso fica visivel durante o beep.
void emitirAlarmeSeNecessario() {
  if (alarmeAtual == SEM_ALARME) return;
  if (somAlarmePendente ||
      (uint32_t)(millis() - ultimoSomAlarme) >= REPETICAO_ALARME_MS) {
    somAlarmePendente = false;
    tocarPadraoAlarme(alarmeAtual);
    ultimoSomAlarme = millis();
  }
}

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

// Verifica um registro completo do formato de eventos. Cabecalho, espacos
// e fragmentos de escrita interrompida nao contam como alerta registrado.
bool linhaContemAlerta(char *linha) {
  char *estado = linha;
  for (uint8_t campo = 0; campo < 3; ++campo) {
    estado = strchr(estado, ',');
    if (!estado) return false;
    ++estado;
  }
  if (strncmp(estado, "ALERTA,", 7) != 0) return false;
  unsigned anterior, novo, leitura;
  char restante;
  // Estado, anterior, novo, leitura e tres valores; rejeita fragmentos.
  float v, i, p;
  return sscanf(estado + 7, "%u,%u,%u,%f,%f,%f%c",
                &anterior, &novo, &leitura, &v, &i, &p, &restante) == 6 &&
         anterior <= FALHA_INA226 && novo > SEM_ALARME &&
         novo <= FALHA_INA226 && leitura <= 1;
}

bool arquivoTemAlerta(const char *nome) {
  if (!SD.exists(nome)) return false;
  File arquivo = SD.open(nome, FILE_READ);
  if (!arquivo) { logAlertasOK = false; return false; }
  uint32_t tamanho = arquivo.size();
  uint32_t inicio = tamanho > JANELA_LEITURA_ALERTAS ?
                    tamanho - JANELA_LEITURA_ALERTAS : 0;
  if (inicio > 0) {
    if (!arquivo.seek(inicio - 1)) {
      arquivo.close(); logAlertasOK = false; return false;
    }
    // Se caiu no meio de uma linha, ignora somente o fragmento inicial.
    if (arquivo.read() != '\n') {
      while (arquivo.available() && arquivo.read() != '\n') yield();
    }
  }
  char linha[192];
  size_t posicao = 0;
  bool excedeu = false, encontrou = false;
  while (arquivo.available()) {
    int c = arquivo.read();
    if (c < 0) { logAlertasOK = false; break; }
    if (c == '\n') {
      linha[posicao] = '\0';
      if (!excedeu && linhaContemAlerta(linha)) { encontrou = true; break; }
      posicao = 0; excedeu = false;
    } else if (c != '\r') {
      if (posicao < sizeof(linha) - 1) linha[posicao++] = (char)c;
      else excedeu = true;
    }
    yield();
  }
  arquivo.close();
  return encontrou;
}

void carregarHistoricoAlertas() {
  historicoAlertas = false;
  if (!cartaoInicializado) return;
  bool atual = arquivoTemAlerta(ARQUIVO_ALERTAS);
  bool anterior = arquivoTemAlerta(ARQUIVO_ALERTAS_ANTERIOR);
  historicoAlertas = atual || anterior;
  Serial.println(historicoAlertas ? "Historico: existem alertas registrados."
                                 : "Historico: sem alertas registrados.");
}

// Mantem somente o arquivo atual e a geracao anterior, ate 1 MiB cada.
// Copia antes de apagar o atual; a API SD usada nao exige rename().
bool rotacionarAlertas() {
  File origem = SD.open(ARQUIVO_ALERTAS, FILE_READ);
  if (!origem) return false;
  uint32_t tamanho = origem.size(), copiados = 0;
  if (SD.exists(ARQUIVO_ALERTAS_ANTERIOR) &&
      !SD.remove(ARQUIVO_ALERTAS_ANTERIOR)) { origem.close(); return false; }
  File destino = SD.open(ARQUIVO_ALERTAS_ANTERIOR, FILE_WRITE);
  if (!destino) { origem.close(); return false; }
  uint8_t bloco[256];
  bool sucesso = true;
  while (copiados < tamanho) {
    size_t pedido = tamanho - copiados;
    if (pedido > sizeof(bloco)) pedido = sizeof(bloco);
    int lidos = origem.read(bloco, pedido);
    if (lidos <= 0 || destino.write(bloco, lidos) != (size_t)lidos) {
      sucesso = false; break;
    }
    copiados += lidos;
    yield();
  }
  destino.close(); origem.close();
  if (!sucesso || copiados != tamanho) return false;
  if (!SD.remove(ARQUIVO_ALERTAS)) return false;
  Serial.println("Rotacao de eventos: ALERTAS.CSV -> ALERTOLD.CSV");
  return true;
}

bool registrarTransicaoAlarme(TipoAlarme anterior, TipoAlarme novo,
                              bool leituraOK, float v, float i) {
  if (!cartaoInicializado) { logAlertasOK = false; return false; }
  char data[32], curto[24], linha[192];
  obterHorario(data, sizeof(data), curto, sizeof(curto));
  int tamanho = snprintf(linha, sizeof(linha),
      "%s,%lu,%s,%s,%u,%u,%u,%.3f,%.6f,%.6f\n", data,
      (unsigned long)millis(), nomeArquivo[0] ? nomeArquivo + 1 : "SEM_CSV",
      novo == SEM_ALARME ? "NORMAL" : "ALERTA",
      (unsigned)anterior, (unsigned)novo, leituraOK ? 1U : 0U,
      leituraOK ? v : 0.0f, leituraOK ? i : 0.0f,
      leituraOK ? v * i : 0.0f);
  if (tamanho <= 0 || tamanho >= (int)sizeof(linha)) {
    logAlertasOK = false; return false;
  }
  const char cabecalho[] =
      "data_hora_local,tempo_ms,arquivo_medicoes,estado,alarme_anterior,"
      "alarme_atual,leitura_ok,tensao_V,corrente_A,potencia_W\n";
  uint32_t tamanhoAtual = 0;
  bool precisaSeparador = false;
  if (SD.exists(ARQUIVO_ALERTAS)) {
    File consulta = SD.open(ARQUIVO_ALERTAS, FILE_READ);
    if (!consulta) { logAlertasOK = false; return false; }
    tamanhoAtual = consulta.size();
    if (tamanhoAtual > 0) {
      if (!consulta.seek(tamanhoAtual - 1)) {
        consulta.close(); logAlertasOK = false; return false;
      }
      precisaSeparador = consulta.read() != '\n';
    }
    consulta.close();
  }
  if (tamanhoAtual + (uint32_t)tamanho + (precisaSeparador ? 1 : 0) >
      MAX_BYTES_ALERTAS) {
    if (!rotacionarAlertas()) { logAlertasOK = false; return false; }
    tamanhoAtual = 0; precisaSeparador = false;
  }
  File arquivo = SD.open(ARQUIVO_ALERTAS, FILE_WRITE);
  if (!arquivo) { logAlertasOK = false; return false; }
  bool sucesso = true;
  if (tamanhoAtual == 0) {
    sucesso = arquivo.write((const uint8_t *)cabecalho, sizeof(cabecalho) - 1)
              == sizeof(cabecalho) - 1;
  } else if (precisaSeparador) {
    // Se houve escrita interrompida, nao mistura a nova linha com a anterior.
    sucesso = arquivo.write((uint8_t)'\n') == 1;
  }
  if (sucesso) sucesso = arquivo.write((const uint8_t *)linha, tamanho)
                        == (size_t)tamanho;
  arquivo.close();
  logAlertasOK = sucesso;
  if (sucesso && novo != SEM_ALARME) historicoAlertas = true;
  return sucesso;
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
  beepInicializacao();
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
  tela.print(sdOK && logAlertasOK ? "SD:OK" : "SD:ERR");
  if (historicoAlertas) {
    tela.setCursor(82, 0);
    tela.print('!');
  }
  desenharWiFi();
  tela.drawFastHLine(4, 11, 120, SSD1306_WHITE);
  // A faixa azul comeca em y=16. Fonte padrao: 14 pixels visiveis
  // na escala 2, mais 2 pixels de espacamento entre as linhas.
  desenharMedicao('V', v, 16, leituraOK);
  desenharMedicao('A', i, 32, leituraOK);
  if (alarmeAtual == SEM_ALARME) {
    desenharMedicao('W', p, 48, leituraOK);
  } else {
    // Mantem V e A grandes. O alerta ocupa a linha inferior de potencia.
    tela.fillRect(4, 48, 120, 8, SSD1306_WHITE);
    tela.setTextSize(1);
    tela.setTextColor(SSD1306_BLACK);
    tela.setCursor(46, 48);
    tela.print("ALERTA");
    tela.setTextColor(SSD1306_WHITE);
    tela.setCursor(4, 56);
    tela.print(mensagemAlarme(alarmeAtual));
  }
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
  cartaoInicializado = SD.begin(CS_SD);
  sdOK = cartaoInicializado;
  carregarHistoricoAlertas();
  if (sdOK) sdOK = criarArquivo();
  if (!sdOK) Serial.println("Falha SD: gravacao desativada.");
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(SSID, SENHA);
  Serial.println("Aguardando Wi-Fi e horario NTP...");
  if (oledOK) mostrarLogo();
  else beepInicializacao();
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
    avaliarAlarme(false, 0, 0);
    atualizarTela(false, 0, 0, 0, horario);
    emitirAlarmeSeNecessario();
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
  Serial.print(sdOK && logAlertasOK ? " | SD: OK" : " | SD: ERRO");
  Serial.println(WiFi.status() == WL_CONNECTED ? " | WiFi: OK"
                                              : " | WiFi: DESCONECTADO");
  avaliarAlarme(true, volts, corrente);
  atualizarTela(true, volts, corrente, potencia, horario);
  emitirAlarmeSeNecessario();
}
