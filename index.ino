#include <Arduino.h>
#include <LittleFS.h>
#include <stddef.h>
#include <string.h>
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

// NodeMCU ESP8266: OLED 0x3C, INA226 0x40, MCP23017 0x20.
// I2C SDA D2 / SCL D1; microSD CS D4 / SCK D5 / MISO D6 / MOSI D7.
// Botoes GPA0 anterior / GPA1 confirmar / GPA2 proximo, para GND.
// LEDs GPB0 vermelho / GPB1 amarelo / GPB2 verde / GPB3 azul, resistor 1k cada.
// Buzzer modulo HIGH em D0: LOW desliga. Alimentacao conforme modulo.
// Configure Flash Size com FS > 0; preserve a mesma particao do teste.
// Configuracoes na LittleFS; CSVs e eventos no SD. Nunca formata automaticamente.
// Limites de alarme sao avisos: este codigo NAO corta a alimentacao da carga.
// Altere as credenciais abaixo. Limites podem ser ajustados no menu.
const char *SSID = "WIFI NAME";
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

// Configuracoes persistentes do monitor (independentes do teste anterior).
struct ConfigMonitor {
  uint32_t magic, versao, geracao;
  uint16_t baixaCV, criticaCV, altaCV, maximaCA;
  uint16_t som, duracaoMs, frequenciaHz, volume;
  uint32_t checksum;
};
uint32_t checksumConfig(const ConfigMonitor &c);
bool validarConfig(const ConfigMonitor &c);
bool mesmosValores(const ConfigMonitor &a, const ConfigMonitor &b);
ConfigMonitor padroesConfig();
bool lerConfig(int slot, ConfigMonitor &c);
bool haAlarmeAtivo();
const char *textoAlarmeAtivo();
enum PaginaMenu { TELA_PRINCIPAL, LISTA_MENU, EDITAR_MENU, RESTAURAR_MENU };
ConfigMonitor configuracao = {}, edicao = {};
PaginaMenu pagina = TELA_PRINCIPAL;
bool fsOK = false, mcpOK = false, configSalva = false, falhaMCP = false;
int slotConfig = -1, itemMenu = 0;
bool telaPendente = true;
bool leituraAtualOK = false;
float tensaoAtual = 0, correnteAtual = 0, potenciaAtual = 0;
char horarioAtual[24] = "NTP aguardando";
const char *avisoMenu = "";
uint32_t avisoMenuEm = 0, mudouBotaoEm[3], ultimaFalhaMCP = 0;
bool botaoLido[3], botaoEstavel[3];
const char *CONFIG_FILES[2] = {"/monitor_a.bin", "/monitor_b.bin"};
// No modo ativo, a frequencia do som e determinada pelo proprio buzzer.
// Se HIGH constante produzir somente um clique, use BUZZER_PASSIVO=true.
const bool BUZZER_PASSIVO = false;
const uint8_t NIVEL_BUZZER_LIGADO = HIGH;
const uint8_t NIVEL_BUZZER_DESLIGADO = LOW;
const char *ITENS_MENU[] = {
  "Tensao baixa", "Tensao critica", "Tensao alta", "Corrente maxima",
  "Som dos alarmes", "Duracao do beep",
  "Frequencia", "Intensidade", "Salvar e sair", "Cancelar", "Restaurar padroes"
};
// Os indices 6 e 7 sao pulados se BUZZER_PASSIVO=false.
const int TOTAL_ITENS = 11;
uint16_t temposSom[9];
uint8_t totalTemposSom = 0, passoSom = 0;
uint32_t passoSomEm = 0;
bool tocandoSom = false;

uint32_t checksumConfig(const ConfigMonitor &c) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&c);
  uint32_t h = 2166136261UL;
  for (size_t n = 0; n < offsetof(ConfigMonitor, checksum); n++) h = (h ^ p[n]) * 16777619UL;
  return h;
}
bool validarConfig(const ConfigMonitor &c) {
  if (c.baixaCV > 1800 || c.criticaCV > 1800 || c.altaCV > 3600 ||
      c.maximaCA > 2000 || c.som > 1 || c.duracaoMs < 50 || c.duracaoMs > 500 ||
      c.frequenciaHz < 500 || c.frequenciaHz > 4000 || c.volume > 50) return false;
  if (c.baixaCV && c.criticaCV && c.criticaCV >= c.baixaCV) return false;
  if (c.altaCV && ((c.baixaCV && c.altaCV <= c.baixaCV) ||
                  (c.criticaCV && c.altaCV <= c.criticaCV))) return false;
  return true;
}
bool mesmosValores(const ConfigMonitor &a, const ConfigMonitor &b) {
  return a.baixaCV == b.baixaCV && a.criticaCV == b.criticaCV &&
    a.altaCV == b.altaCV && a.maximaCA == b.maximaCA && a.som == b.som &&
    a.duracaoMs == b.duracaoMs && a.frequenciaHz == b.frequenciaHz && a.volume == b.volume;
}
ConfigMonitor padroesConfig() {
  ConfigMonitor c = {};
  // Limites 0 = desativados, preservando o comportamento do codigo recebido.
  c.som = 1; c.duracaoMs = 150; c.frequenciaHz = 3000; c.volume = 20;
  return c;
}
bool lerConfig(int slot, ConfigMonitor &c) {
  File f = LittleFS.open(CONFIG_FILES[slot], "r");
  if (!f) return false;
  bool ok = f.size() == sizeof(c) && f.read(reinterpret_cast<uint8_t *>(&c), sizeof(c)) == sizeof(c);
  f.close();
  return ok && c.magic == 0x4D4F4E49UL && c.versao == 1 &&
    validarConfig(c) && c.checksum == checksumConfig(c);
}
void carregarConfiguracao() {
  LittleFSConfig cfg; cfg.setAutoFormat(false); LittleFS.setConfig(cfg);
  fsOK = LittleFS.begin(); configuracao = padroesConfig(); slotConfig = -1;
  ConfigMonitor a = {}, b = {};
  bool va = fsOK && lerConfig(0, a), vb = fsOK && lerConfig(1, b);
  if (va && (!vb || static_cast<int32_t>(a.geracao - b.geracao) > 0)) {
    configuracao = a; slotConfig = 0;
  } else if (vb) { configuracao = b; slotConfig = 1; }
  configSalva = slotConfig >= 0; edicao = configuracao;
  Serial.println(!fsOK ? "LittleFS: erro de montagem; usando padroes, salvar indisponivel."
    : configSalva ? "LittleFS: configuracao recuperada." : "LittleFS: padroes; nenhum ajuste do monitor salvo.");
}
bool salvarConfiguracao() {
  if (!fsOK || !validarConfig(edicao)) return false;
  if (configSalva && mesmosValores(edicao, configuracao)) return true;
  ConfigMonitor nova = edicao;
  nova.magic = 0x4D4F4E49UL; nova.versao = 1; nova.geracao = configuracao.geracao + 1;
  nova.checksum = checksumConfig(nova);
  int destino = slotConfig == 0 ? 1 : 0;
  File f = LittleFS.open(CONFIG_FILES[destino], "w");
  if (!f) return false;
  size_t escritos = f.write(reinterpret_cast<const uint8_t *>(&nova), sizeof(nova));
  f.flush(); f.close();
  ConfigMonitor conferida = {};
  if (escritos != sizeof(nova) || !lerConfig(destino, conferida) ||
      memcmp(&nova, &conferida, sizeof(nova)) != 0) return false;
  configuracao = conferida; edicao = configuracao; slotConfig = destino; configSalva = true;
  Serial.printf("Ajustes salvos: baixa=%.2f critica=%.2f alta=%.2f maxima=%.2f som=%u beep=%u ms\n",
    configuracao.baixaCV / 100.0, configuracao.criticaCV / 100.0,
    configuracao.altaCV / 100.0, configuracao.maximaCA / 100.0,
    configuracao.som, configuracao.duracaoMs);
  return true;
}
bool escreverMCP(uint8_t reg, uint8_t valor) {
  Wire.beginTransmission(0x20); Wire.write(reg); Wire.write(valor);
  return Wire.endTransmission() == 0;
}
bool lerMCP(uint8_t &valor) {
  Wire.beginTransmission(0x20); Wire.write(0x12);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)0x20, (uint8_t)1) != 1) return false;
  valor = Wire.read(); return true;
}
void iniciarMCP() {
  // BANK=0, padrao apos energizar. A0-A2 de endereco em GND, RESET em 3V3.
  bool ok = escreverMCP(0x0A, 0);
  ok = escreverMCP(0x00, 0xFF) && ok;
  ok = escreverMCP(0x02, 0) && ok;
  ok = escreverMCP(0x0C, 0x07) && ok;
  ok = escreverMCP(0x15, 0) && ok;
  ok = escreverMCP(0x01, 0xF0) && ok;
  uint8_t valor = 0xFF; mcpOK = lerMCP(valor) && ok;
  for (uint8_t n = 0; n < 3; n++) {
    botaoLido[n] = botaoEstavel[n] = (valor & (1 << n)) != 0;
    mudouBotaoEm[n] = millis();
  }
  Serial.println(mcpOK ? "MCP23017: OK." : "MCP23017: falha, monitor continua sem menu.");
}
void avisarMenu(const char *texto) {
  avisoMenu = texto; avisoMenuEm = millis(); telaPendente = true; Serial.println(texto);
}
int proximoItem(int item, int sentido) {
  do { item = (item + sentido + TOTAL_ITENS) % TOTAL_ITENS; }
  while (!BUZZER_PASSIVO && (item == 6 || item == 7));
  return item;
}
String valorMenu() {
  uint16_t valor = itemMenu == 0 ? edicao.baixaCV : itemMenu == 1 ? edicao.criticaCV :
    itemMenu == 2 ? edicao.altaCV : edicao.maximaCA;
  if (itemMenu <= 3) {
    if (!valor) return "DESATIVADO";
    return String(valor / 100.0, 2) + (itemMenu == 3 ? " A" : " V");
  }
  if (itemMenu == 4) return edicao.som ? "LIGADO" : "DESLIGADO";
  if (itemMenu == 5) return String(edicao.duracaoMs) + " ms";
  if (itemMenu == 6) return String(edicao.frequenciaHz) + " Hz";
  return String(edicao.volume) + "%";
}
void ajustarItem(int sentido) {
  int valor = 0, limite = 0;
  if (itemMenu <= 3) {
    uint16_t *alvo = itemMenu == 0 ? &edicao.baixaCV : itemMenu == 1 ? &edicao.criticaCV :
      itemMenu == 2 ? &edicao.altaCV : &edicao.maximaCA;
    limite = itemMenu == 2 ? 3600 : itemMenu == 3 ? 2000 : 1800;
    valor = *alvo;
    if (valor == 0 && sentido > 0) valor = itemMenu == 3 ? 100 : itemMenu == 2 ? 1500 : itemMenu == 1 ? 1050 : 1150;
    else if (sentido < 0 && valor <= (itemMenu == 3 ? 10 : 800)) valor = 0;
    else valor = constrain(valor + sentido * 10, 0, limite);
    *alvo = valor;
  } else if (itemMenu == 4) edicao.som = !edicao.som;
  else if (itemMenu == 5) edicao.duracaoMs = constrain((int)edicao.duracaoMs + sentido * 25, 50, 500);
  else if (itemMenu == 6) edicao.frequenciaHz = constrain((int)edicao.frequenciaHz + sentido * 100, 500, 4000);
  else edicao.volume = constrain((int)edicao.volume + sentido, 0, 50);
}
void botaoPressionado(uint8_t n) {
  telaPendente = true;
  if (pagina == TELA_PRINCIPAL) {
    if (n == 1) { edicao = configuracao; itemMenu = 0; pagina = LISTA_MENU; }
  } else if (pagina == LISTA_MENU) {
    if (n != 1) itemMenu = proximoItem(itemMenu, n == 0 ? -1 : 1);
    else if (itemMenu < 8) pagina = EDITAR_MENU;
    else if (itemMenu == 8) {
      if (!validarConfig(edicao)) avisarMenu("Limites inconsistentes");
      else if (!salvarConfiguracao()) avisarMenu("Falha salvar LittleFS");
      else { pagina = TELA_PRINCIPAL; avisarMenu("Configuracao salva"); }
    } else if (itemMenu == 9) { edicao = configuracao; pagina = TELA_PRINCIPAL; avisarMenu("Edicao cancelada"); }
    else pagina = RESTAURAR_MENU;
  } else if (pagina == EDITAR_MENU) {
    if (n == 1) pagina = LISTA_MENU; else ajustarItem(n == 0 ? -1 : 1);
  } else {
    if (n == 1) { edicao = padroesConfig(); avisarMenu("Padroes: falta salvar"); }
    pagina = LISTA_MENU;
  }
}
void verificarBotoes() {
  if (!mcpOK) return;
  static int limiteSegurado = -1;
  static bool desativouNestaPressao = false;
  uint8_t entradas; uint32_t agora = millis();
  if (!lerMCP(entradas)) {
    if (!falhaMCP) { falhaMCP = true; telaPendente = true; Serial.println("Falha I2C MCP23017."); }
    // Requer nova leitura estavel depois de uma interrupcao.
    for (uint8_t n = 0; n < 3; n++) mudouBotaoEm[n] = agora;
    return;
  }
  if (falhaMCP) { falhaMCP = false; telaPendente = true; }
  for (uint8_t n = 0; n < 3; n++) {
    bool nivel = (entradas & (1 << n)) != 0;
    if (nivel != botaoLido[n]) { botaoLido[n] = nivel; mudouBotaoEm[n] = agora; }
    if (nivel != botaoEstavel[n] && agora - mudouBotaoEm[n] >= 35) {
      botaoEstavel[n] = nivel;
      if (n == 0) {
        limiteSegurado = !nivel && pagina == EDITAR_MENU && itemMenu <= 3 ? itemMenu : -1;
        desativouNestaPressao = false;
      }
      if (!nivel) botaoPressionado(n);
    }
  }
  // Pressao longa no Anterior desativa somente o limite em edicao.
  // A mudanca continua pendente: Salvar aplica; Cancelar descarta.
  if (!botaoEstavel[0] && !botaoLido[0] && !desativouNestaPressao &&
      pagina == EDITAR_MENU && limiteSegurado == itemMenu && itemMenu <= 3 &&
      agora - mudouBotaoEm[0] >= 1200) {
    uint16_t *alvo = itemMenu == 0 ? &edicao.baixaCV : itemMenu == 1 ? &edicao.criticaCV :
      itemMenu == 2 ? &edicao.altaCV : &edicao.maximaCA;
    *alvo = 0;
    desativouNestaPressao = true;
    avisarMenu("Limite desativado");
  }
  if (avisoMenu[0] && agora - avisoMenuEm >= 2500) { avisoMenu = ""; telaPendente = true; }
}
void desenharMenu() {
  if (!oledOK) return;
  tela.clearDisplay(); tela.setTextColor(SSD1306_WHITE); tela.setTextSize(1);
  tela.setCursor(0, 0); tela.print("CONFIG");
  if (haAlarmeAtivo()) { tela.setCursor(51, 0); tela.print('!'); }
  tela.setCursor(67, 0); tela.print(fsOK ? "FS:OK" : "FS:ERR");
  if (!mesmosValores(configuracao, edicao)) { tela.setCursor(121, 0); tela.print('*'); }
  tela.drawFastHLine(0, 11, 128, SSD1306_WHITE);
  tela.setCursor(0, 16);
  if (pagina == LISTA_MENU) {
    // Janela de 4 itens, sempre incluindo o selecionado.
    int indice = itemMenu;
    for (uint8_t n = 0; n < 4; n++) {
      tela.print(n == 0 ? ">" : " "); tela.println(ITENS_MENU[indice]);
      indice = proximoItem(indice, 1);
    }
  } else if (pagina == EDITAR_MENU) {
    tela.println(ITENS_MENU[itemMenu]);
    String valor = valorMenu(); tela.setTextSize(valor.length() > 10 ? 1 : 2);
    tela.setCursor(0, 29); tela.print(valor); tela.setTextSize(1);
    tela.setCursor(0, 47); tela.print("Anterior - Proximo +");
  } else {
    tela.println("Restaurar padroes?"); tela.println("Confirmar: sim");
    tela.println("Anterior/Proximo: nao"); tela.println("Depois escolha Salvar");
  }
  tela.setCursor(0, 56);
  if (avisoMenu[0]) tela.print(avisoMenu);
  else if (haAlarmeAtivo()) tela.print(textoAlarmeAtivo());
  else if (pagina == EDITAR_MENU && itemMenu <= 3) tela.print("Segure Anterior: OFF");
  else tela.print("Confirmar: selecionar");
  tela.display();
}

const uint32_t REPETICAO_ALARME_MS = 15000UL;
const uint32_t CONFIRMACAO_CONDICAO_MS = 3000UL;
const float HISTERESE_TENSAO_V = 0.20f;
const float HISTERESE_CORRENTE_A = 0.05f;

enum TipoAlarme {
  SEM_ALARME, BATERIA_BAIXA, BATERIA_CRITICA,
  ERRO_SD, SOBRECORRENTE, FALHA_INA226, TENSAO_ALTA
};
const char *mensagemAlarme(TipoAlarme tipo);
void tocarPadraoAlarme(TipoAlarme tipo);
TipoAlarme detectarAlarme(bool leituraOK, float v, float i);
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
// Sequencia de duracoes ON/OFF atendida pelo loop, sem delay de alarme.
void silenciarBuzzer() {
  if (BUZZER_PASSIVO) analogWrite(BUZZER, 0);
  digitalWrite(BUZZER, NIVEL_BUZZER_DESLIGADO);
}
void acionarBuzzer(bool ligado, bool inicializacao = false) {
  if (!ligado) { silenciarBuzzer(); return; }
  if (BUZZER_PASSIVO) {
    uint16_t hz = inicializacao ? 3500 : configuracao.frequenciaHz;
    uint8_t intensidade = inicializacao ? 12 : configuracao.volume;
    analogWriteRange(1000); analogWriteFreq(hz);
    // PWM do modulo HIGH. Intensidade e duty cycle, nao volume em dB.
    analogWrite(BUZZER, intensidade * 10);
  } else digitalWrite(BUZZER, NIVEL_BUZZER_LIGADO);
}
void pararSom() {
  if (tocandoSom) telaPendente = true;
  tocandoSom = false; totalTemposSom = 0; silenciarBuzzer();
}
void iniciarSom(const uint16_t *tempos, uint8_t quantidade) {
  pararSom();
  if (!configuracao.som || quantidade == 0 || quantidade > 9) return;
  memcpy(temposSom, tempos, quantidade * sizeof(uint16_t));
  totalTemposSom = quantidade; passoSom = 0;
  passoSomEm = millis(); tocandoSom = true; telaPendente = true; acionarBuzzer(true);
}
void atualizarSom() {
  if (!tocandoSom) return;
  if (!configuracao.som || alarmeAtual == SEM_ALARME) { pararSom(); return; }
  if ((uint32_t)(millis() - passoSomEm) < temposSom[passoSom]) return;
  ++passoSom;
  if (passoSom >= totalTemposSom) { pararSom(); return; }
  passoSomEm = millis(); acionarBuzzer((passoSom % 2) == 0);
}
void beepInicializacao() {
  if (!configuracao.som) return;
  acionarBuzzer(true, true); delay(80); silenciarBuzzer();
}
const char *mensagemAlarme(TipoAlarme tipo) {
  switch (tipo) {
    case BATERIA_BAIXA: return "BATERIA BAIXA";
    case BATERIA_CRITICA: return "BATERIA CRITICA";
    case ERRO_SD: return "FALHA NO CARTAO SD";
    case SOBRECORRENTE: return "SOBRECORRENTE";
    case FALHA_INA226: return "FALHA INA226";
    case TENSAO_ALTA: return "TENSAO ALTA";
    default: return "";
  }
}
bool haAlarmeAtivo() { return alarmeAtual != SEM_ALARME; }
const char *textoAlarmeAtivo() { return mensagemAlarme(alarmeAtual); }
void tocarPadraoAlarme(TipoAlarme tipo) {
  uint16_t b = configuracao.duracaoMs;
  // Alarmes identificados pela quantidade e duracao, independentemente do tom.
  const uint16_t baixa[] = {b, b, b};                        // 2 curtos
  const uint16_t critica[] = {b, b, b, b, b};               // 3 curtos
  const uint16_t sd[] = {(uint16_t)(3*b), b, b};            // longo + curto
  const uint16_t corrente[] = {(uint16_t)(4*b), (uint16_t)(4*b), (uint16_t)(4*b)};
  const uint16_t sensor[] = {b,b,b,b,b,b,b};                 // 4 curtos
  const uint16_t alta[] = {b,b,(uint16_t)(3*b)};             // curto + longo
  switch (tipo) {
    case BATERIA_BAIXA: iniciarSom(baixa, 3); break;
    case BATERIA_CRITICA: iniciarSom(critica, 5); break;
    case ERRO_SD: iniciarSom(sd, 3); break;
    case SOBRECORRENTE: iniciarSom(corrente, 3); break;
    case FALHA_INA226: iniciarSom(sensor, 7); break;
    case TENSAO_ALTA: iniciarSom(alta, 3); break;
    default: pararSom(); break;
  }
}

TipoAlarme detectarAlarme(bool leituraOK, float v, float i) {
  if (ALARME_TESTE != SEM_ALARME) return ALARME_TESTE;
  // Prioridade: sensor, sobrecorrente, critica, tensao alta, SD, baixa.
  if (!leituraOK) return FALHA_INA226;
  float margemI = alarmeAtual == SOBRECORRENTE ? HISTERESE_CORRENTE_A : 0;
  if ((configuracao.maximaCA / 100.0f) > 0 && fabsf(i) >= (configuracao.maximaCA / 100.0f) - margemI)
    return SOBRECORRENTE;
  float margemCritica = alarmeAtual == BATERIA_CRITICA ? HISTERESE_TENSAO_V : 0;
  if ((configuracao.criticaCV / 100.0f) > 0 && v <= (configuracao.criticaCV / 100.0f) + margemCritica)
    return BATERIA_CRITICA;
  float margemAlta = alarmeAtual == TENSAO_ALTA ? HISTERESE_TENSAO_V : 0;
  if (configuracao.altaCV > 0 && v >= configuracao.altaCV / 100.0f - margemAlta)
    return TENSAO_ALTA;
  if (!sdOK || !logAlertasOK) return ERRO_SD;
  float margemBaixa = alarmeAtual == BATERIA_BAIXA ? HISTERESE_TENSAO_V : 0;
  if ((configuracao.baixaCV / 100.0f) > 0 && v <= (configuracao.baixaCV / 100.0f) + margemBaixa)
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
    pararSom();
    telaPendente = true;
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
  if (alarmeAtual == SEM_ALARME || !configuracao.som || tocandoSom) return;
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
         anterior <= TENSAO_ALTA && novo > SEM_ALARME &&
         novo <= TENSAO_ALTA && leitura <= 1;
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
  digitalWrite(BUZZER, NIVEL_BUZZER_DESLIGADO);
  pinMode(BUZZER, OUTPUT);
  Serial.begin(115200);
  delay(500);
  Wire.begin(D2, D1);
  carregarConfiguracao();
  Wire.setClock(100000);
  oledOK = tela.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledOK) Serial.println("Falha ao inicializar OLED.");
  Wire.setClock(100000);
  iniciarMCP();
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
  if (SSID[0]) WiFi.begin(SSID, SENHA);
  Serial.println("Aguardando Wi-Fi e horario NTP...");
  if (oledOK) mostrarLogo();
  else beepInicializacao();
  atualizarTela(false, 0, 0, 0, "NTP aguardando");
  ultimaMedicao = millis();
}

// Vermelho: alarme/falha local; amarelo: historico ou edicao pendente;
// verde: leitura valida sem alarme; azul: Wi-Fi conectado.
void atualizarLEDs() {
  if (!mcpOK || falhaMCP) return;
  uint8_t leds = 0;
  if (alarmeAtual != SEM_ALARME || !fsOK) leds |= 0x01;
  if (historicoAlertas || !mesmosValores(configuracao, edicao)) leds |= 0x02;
  if (leituraAtualOK && alarmeAtual == SEM_ALARME) leds |= 0x04;
  if (WiFi.status() == WL_CONNECTED) leds |= 0x08;
  static uint8_t anterior = 0xFF;
  if (leds != anterior && escreverMCP(0x15, leds)) anterior = leds;
}
void apresentarTela() {
  // O alerta nao retira o usuario do menu. No menu aparece ! e o texto
  // no rodape; som, medicoes e registros continuam ativos.
  if (pagina == TELA_PRINCIPAL)
    atualizarTela(leituraAtualOK, tensaoAtual, correnteAtual, potenciaAtual, horarioAtual);
  else desenharMenu();
  telaPendente = false;
}
void loop() {
  verificarWiFi(); verificarBotoes(); atualizarSom();
  uint32_t agora = millis();
  static uint32_t ultimaTela = 0;
  if ((uint32_t)(agora - ultimaMedicao) >= INTERVALO_MS) {
    ultimaMedicao = agora;
    char data[32];
    bool horaOK = obterHorario(data, sizeof(data), horarioAtual, sizeof(horarioAtual));
    if (horaOK && !horaAnunciada) {
      horaAnunciada = true; Serial.print("Hora sincronizada: "); Serial.println(data);
    }
    uint16_t brutoBarramento, brutoShunt;
    leituraAtualOK = lerRegistro16(0x02, brutoBarramento) && lerRegistro16(0x01, brutoShunt);
    if (!leituraAtualOK) {
      tensaoAtual = correnteAtual = potenciaAtual = 0;
      Serial.println("Falha ao ler INA226.");
    } else {
      tensaoAtual = brutoBarramento * 0.00125f;
      float voltsShunt = (int16_t)brutoShunt * 0.0000025f;
      correnteAtual = voltsShunt / RESISTENCIA_SHUNT;
      potenciaAtual = tensaoAtual * correnteAtual;
      if (sdOK && !gravarMedicao(data, agora, tensaoAtual, voltsShunt * 1000.0f,
                                correnteAtual, potenciaAtual)) {
        sdOK = false; Serial.println("Falha SD: gravacao interrompida. Verifique o cartao.");
      }
      Serial.printf("%s | Tensao: %.3f V | Corrente: %.3f A | Potencia: %.3f W | SD: %s | WiFi: %s | FS: %s\n",
        data, tensaoAtual, correnteAtual, potenciaAtual,
        sdOK && logAlertasOK ? "OK" : "ERRO", WiFi.status() == WL_CONNECTED ? "OK" : "DESCONECTADO",
        fsOK ? "OK" : "ERRO");
    }
    avaliarAlarme(leituraAtualOK, tensaoAtual, correnteAtual);
    telaPendente = true;
  }
  if (telaPendente || agora - ultimaTela >= 200) {
    apresentarTela(); ultimaTela = agora;
  }
  emitirAlarmeSeNecessario(); atualizarLEDs();
  delay(1);
}
