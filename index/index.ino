// Correcao de temporizacao: pausas reais no logo e atendimento periodico ao SDK.
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
const char *SSID = "WIFI_NAME";
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
struct ConfigMonitorV1 {
  uint32_t magic, versao, geracao;
  uint16_t baixaCV, criticaCV, altaCV, maximaCA;
  uint16_t som, duracaoMs, frequenciaHz, volume;
  uint32_t checksum;
};
uint32_t checksumV1(const ConfigMonitorV1 &c);
struct ConfigMonitorV2 {
  uint32_t magic, versao, geracao;
  uint16_t baixaCV, criticaCV, altaCV, maximaCA;
  uint16_t som, duracaoMs, frequenciaHz, volume;
  uint16_t rotacaoSeg, reservado;
  uint32_t checksum;
};
uint32_t checksumV2(const ConfigMonitorV2 &c);
struct ConfigMonitorV3 {
  uint32_t magic, versao, geracao;
  uint16_t baixaCV, criticaCV, altaCV, maximaCA;
  uint16_t som, duracaoMs, frequenciaHz, volume;
  uint16_t rotacaoSeg, reservado;
  uint16_t brilhoLED[4]; // Legado da versao 3: reservado, sem efeito nos LEDs.
  uint32_t checksum;
};
struct ConfigMonitor {
  uint32_t magic, versao, geracao;
  uint16_t baixaCV, criticaCV, altaCV, maximaCA;
  uint16_t som, duracaoMs, frequenciaHz, volume;
  uint16_t rotacaoSeg, reservado;
  uint16_t brilhoLED[4]; // Legado da versao 3: reservado, sem efeito nos LEDs.
  uint16_t capacidadeAh, reservaPct, perfilBat, eficienciaPct;
  uint32_t identidadeBateria; // Muda somente ao trocar capacidade/perfil.
  uint32_t checksum;
};
uint32_t checksumConfig(const ConfigMonitor &c);
bool validarConfig(const ConfigMonitor &c);
bool mesmosValores(const ConfigMonitor &a, const ConfigMonitor &b);
ConfigMonitor padroesConfig();
bool lerConfig(int slot, ConfigMonitor &c);
bool haAlarmeAtivo();
const char *textoAlarmeAtivo();
bool reconhecerAlertas();
void navegarTela(int sentido);
void entrarMenuTela();
void notificarAlarmeTela();
void atualizarNavegacao();
void atualizarLEDs();
void enviarTela();
void manterBateria();
void textoAutonomia(char *texto,size_t n);
void carregarBateria();
void atualizarContagem(bool valida, float v, float i, uint32_t agora);
bool salvarBateria();
bool sincronizarBateria();
void configuracaoBateriaMudou();
float percentualBateria();
void desenharTelaBateria();
enum Vista { VISTA_RESUMO, VISTA_TENSAO, VISTA_CORRENTE, VISTA_POTENCIA, VISTA_BATERIA, VISTA_ALERTA };
Vista vista = VISTA_RESUMO, vistaRetorno = VISTA_RESUMO;
bool alertaForcado = false, alertaTelaPendente = false;
uint32_t vistaEm = 0, cicloAlertaEm = 0;
uint32_t ultimaInteracaoMenu = 0;
const uint32_t INATIVIDADE_MENU_MS = 20000UL;
const uint32_t PERIODO_TELA_ALERTA_MS = 30000UL;
enum PaginaMenu { TELA_PRINCIPAL, LISTA_MENU, EDITAR_MENU, RESTAURAR_MENU, RECONHECER_MENU, SINCRONIZAR_MENU };
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
  "Frequencia", "Intensidade", "Salvar e sair", "Cancelar", "Restaurar padroes",
  "Reconhecer alertas", "Rotacao de telas",
  "Bateria: Ah", "Perfil bateria", "Reserva minima", "Eficiencia recarga", "Bateria cheia"
};
// Os indices 6 e 7 sao pulados se BUZZER_PASSIVO=false.
const int TOTAL_ITENS = 18;
uint16_t temposSom[9];
uint8_t totalTemposSom = 0, passoSom = 0;
uint32_t passoSomEm = 0;
bool tocandoSom = false;

// BEGIN AH_MATH: independente de Wi-Fi, NTP, SD e LittleFS.
struct EstadoBateria {
  uint32_t magic, versao, geracao;
  uint16_t capacidadeAh, perfilBat, sincronizada, reservado;
  uint32_t alinhamento;
  double restanteAh, retiradosAh, recebidosAh;
  uint32_t identidadeBateria, checksum;
};
void integrarAh(EstadoBateria &e, double anterior, double atual, uint32_t dtMs, double eficiencia);
uint32_t checksumBateria(const EstadoBateria &e);
bool lerBateria(int slot,EstadoBateria &e);
void integrarAh(EstadoBateria &e, double anterior, double atual, uint32_t dtMs, double eficiencia) {
  double horas=dtMs/3600000.0, entrada=0, saida=0;
  if(anterior>=0 && atual>=0) entrada=(anterior+atual)*0.5*horas;
  else if(anterior<=0 && atual<=0) saida=-(anterior+atual)*0.5*horas;
  else {
    double fracao=fabs(anterior)/(fabs(anterior)+fabs(atual));
    if(anterior>0) {entrada=anterior*0.5*horas*fracao;saida=-atual*0.5*horas*(1-fracao);}
    else {saida=-anterior*0.5*horas*fracao;entrada=atual*0.5*horas*(1-fracao);}
  }
  e.retiradosAh+=saida;e.recebidosAh+=entrada;
  if(e.sincronizada) {
    e.restanteAh+=entrada*eficiencia-saida;
    if(e.restanteAh<0) e.restanteAh=0;
    if(e.restanteAh>e.capacidadeAh) e.restanteAh=e.capacidadeAh;
  }
}
// END AH_MATH
EstadoBateria bateria={};
bool bateriaRetomada=false,bateriaPersistenciaOK=true,bateriaSuja=false;
int slotBateria=-1;
uint32_t ultimoSaveBateria=0,ultimaAmostraBateria=0,tempoMediaBateria=0;
bool amostraBateriaAnterior=false;
float correnteAnteriorBateria=0,mediaCorrenteBateria=0;
// Filtro de zero APENAS para integracao: ajustar apos afericao do offset.
const float ZONA_ZERO_A=0.010f;
const uint32_t SALVAR_BATERIA_MS=300000UL,MAX_LACUNA_MS=5000UL;
const char *BAT_FILES[2]={"/bat_a.bin","/bat_b.bin"};
uint32_t checksumBateria(const EstadoBateria &e) {
  const uint8_t *p=reinterpret_cast<const uint8_t*>(&e);uint32_t h=2166136261UL;
  for(size_t n=0;n<offsetof(EstadoBateria,checksum);n++) h=(h^p[n])*16777619UL;
  return h;
}
bool lerBateria(int slot,EstadoBateria &e) {
  File f=LittleFS.open(BAT_FILES[slot],"r");if(!f) return false;
  bool ok=f.size()==sizeof(e) && f.read(reinterpret_cast<uint8_t*>(&e),sizeof(e))==sizeof(e);f.close();
  return ok && e.magic==0x42415441UL && e.versao==1 && e.checksum==checksumBateria(e) &&
    e.capacidadeAh>=1 && e.capacidadeAh<=1000 && e.perfilBat<=1 && e.sincronizada<=1 &&
    isfinite(e.restanteAh) && isfinite(e.retiradosAh) && isfinite(e.recebidosAh) &&
    e.restanteAh>=0 && e.restanteAh<=e.capacidadeAh && e.retiradosAh>=0 && e.recebidosAh>=0;
}
bool salvarBateria() {
  ultimoSaveBateria=millis();
  if(!fsOK) {bateriaPersistenciaOK=false;return false;}
  EstadoBateria nova=bateria;
  nova.magic=0x42415441UL;nova.versao=1;nova.geracao=bateria.geracao+1;
  nova.checksum=checksumBateria(nova);
  int destino=slotBateria==0?1:0;
  File f=LittleFS.open(BAT_FILES[destino],"w");
  if(!f) {bateriaPersistenciaOK=false;return false;}
  size_t n=f.write(reinterpret_cast<const uint8_t*>(&nova),sizeof(nova));f.flush();f.close();
  EstadoBateria teste={};
  bool ok=n==sizeof(nova) && lerBateria(destino,teste) && memcmp(&nova,&teste,sizeof(nova))==0;
  bateriaPersistenciaOK=ok;
  if(ok) {bateria=nova;slotBateria=destino;bateriaSuja=false;}
  else Serial.println("Bateria: falha ao salvar checkpoint LittleFS.");
  return ok;
}
void configuracaoBateriaMudou() {
  uint32_t geracao=bateria.geracao;
  bateria={};bateria.geracao=geracao;
  bateria.capacidadeAh=configuracao.capacidadeAh;bateria.perfilBat=configuracao.perfilBat;
  bateria.identidadeBateria=configuracao.identidadeBateria;
  bateriaRetomada=false;amostraBateriaAnterior=false;tempoMediaBateria=0;mediaCorrenteBateria=0;
  bateriaSuja=true;salvarBateria();
  Serial.println("Bateria: nova capacidade/perfil; SOC desconhecido. Sincronize apos carga completa.");
}
void carregarBateria() {
  bateria={};bateria.capacidadeAh=configuracao.capacidadeAh;bateria.perfilBat=configuracao.perfilBat;
  EstadoBateria a={},b={};
  bool va=fsOK && lerBateria(0,a),vb=fsOK && lerBateria(1,b);
  if(va && (!vb || static_cast<int32_t>(a.geracao-b.geracao)>0)) {bateria=a;slotBateria=0;}
  else if(vb) {bateria=b;slotBateria=1;}
  if(bateria.capacidadeAh!=configuracao.capacidadeAh || bateria.perfilBat!=configuracao.perfilBat ||
     bateria.identidadeBateria!=configuracao.identidadeBateria) {
    configuracaoBateriaMudou();return;
  }
  bateriaRetomada=bateria.sincronizada;
  bateriaPersistenciaOK=fsOK;ultimoSaveBateria=millis();
  if(bateriaRetomada) Serial.println("Bateria: estimativa retomada; intervalo com monitor desligado nao contabilizado.");
  else Serial.println("Bateria: carga desconhecida; contagem de Ah ativa apos leitura valida.");
}
void invalidarContagem() {
  amostraBateriaAnterior=false;tempoMediaBateria=0;mediaCorrenteBateria=0;
  if(bateria.sincronizada) {
    bateria.sincronizada=0;bateriaSuja=true;salvarBateria();
    Serial.println("Bateria: lacuna de medicao. SOC invalidado; sincronize novamente.");
  }
}
void atualizarContagem(bool valida,float v,float i,uint32_t agora) {
  if(!valida || !isfinite(v) || !isfinite(i) || v<2.0f) {invalidarContagem();return;}
  float corrente=fabsf(i)<ZONA_ZERO_A?0:i;
  if(amostraBateriaAnterior) {
    uint32_t dt=agora-ultimaAmostraBateria;
    if(dt>MAX_LACUNA_MS) invalidarContagem();
    else if(dt) {
      integrarAh(bateria,correnteAnteriorBateria,corrente,dt,configuracao.eficienciaPct/100.0);
      float alfa=dt/(60000.0f+dt);
      mediaCorrenteBateria+=(corrente-mediaCorrenteBateria)*alfa;
      if(tempoMediaBateria<60000) tempoMediaBateria+=dt;
      if(corrente || correnteAnteriorBateria) bateriaSuja=true;
    }
  }
  if(!amostraBateriaAnterior) mediaCorrenteBateria=corrente;
  correnteAnteriorBateria=corrente;ultimaAmostraBateria=agora;amostraBateriaAnterior=true;
}
void manterBateria() {
  uint32_t prazo=bateriaPersistenciaOK?SALVAR_BATERIA_MS:60000UL;
  if(bateriaSuja && millis()-ultimoSaveBateria>=prazo) salvarBateria();
}
bool sincronizarBateria() {
  if(!fsOK || !leituraAtualOK || tensaoAtual<2 || !amostraBateriaAnterior ||
     millis()-ultimaAmostraBateria>MAX_LACUNA_MS) return false;
  EstadoBateria anterior=bateria;
  bateria.restanteAh=configuracao.capacidadeAh;
  bateria.retiradosAh=bateria.recebidosAh=0;bateria.sincronizada=1;bateriaSuja=true;
  if(!salvarBateria()) {bateria=anterior;bateriaSuja=true;return false;}
  bateriaRetomada=false;
  Serial.println("Bateria: referencia 100% confirmada pelo usuario. Contadores do ciclo zerados.");
  return true;
}
float percentualBateria() {
  if(!bateria.sincronizada) return -1;
  return constrain((float)(100.0*bateria.restanteAh/configuracao.capacidadeAh),0.0f,100.0f);
}
void textoAutonomia(char *texto,size_t n) {
  if(percentualBateria()<0) {snprintf(texto,n,"SEM REFERENCIA");return;}
  if(!amostraBateriaAnterior || !leituraAtualOK) {snprintf(texto,n,"SEM MEDICAO");return;}
  if(correnteAtual>0.05f) {snprintf(texto,n,"CARREGANDO");return;}
  if(tempoMediaBateria<60000) {snprintf(texto,n,"AGUARDE 1 MIN");return;}
  if(mediaCorrenteBateria>=-0.05f) {snprintf(texto,n,"SEM ESTIMATIVA");return;}
  double uteis=bateria.restanteAh-configuracao.capacidadeAh*configuracao.reservaPct/100.0;
  double minutos=fmax(0.0,uteis)/(-mediaCorrenteBateria)*60;
  if(minutos>59999) {snprintf(texto,n,">999h EST.");return;}
  uint32_t m=(uint32_t)minutos;snprintf(texto,n,"%luh%02lu EST.",(unsigned long)(m/60),(unsigned long)(m%60));
}

uint32_t checksumConfig(const ConfigMonitor &c) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&c);
  uint32_t h = 2166136261UL;
  for (size_t n = 0; n < offsetof(ConfigMonitor, checksum); n++) h = (h ^ p[n]) * 16777619UL;
  return h;
}
uint32_t checksumV1(const ConfigMonitorV1 &c) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&c);
  uint32_t h = 2166136261UL;
  for (size_t n = 0; n < offsetof(ConfigMonitorV1, checksum); n++) h = (h ^ p[n]) * 16777619UL;
  return h;
}
uint32_t checksumV2(const ConfigMonitorV2 &c) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&c);
  uint32_t h = 2166136261UL;
  for (size_t n=0;n<offsetof(ConfigMonitorV2,checksum);n++) h=(h^p[n])*16777619UL;
  return h;
}
bool validarConfig(const ConfigMonitor &c) {
  for(uint8_t n=0;n<4;n++) {
    uint16_t b=c.brilhoLED[n];
    if(b!=5 && (b<10 || b>100 || b%10!=0)) return false;
  }
  if(c.capacidadeAh<1 || c.capacidadeAh>1000 || c.reservaPct>90 ||
     c.perfilBat>1 || c.eficienciaPct<50 || c.eficienciaPct>100) return false;
  if (c.baixaCV > 1800 || c.criticaCV > 1800 || c.altaCV > 3600 ||
      c.maximaCA > 2000 || c.som > 1 || c.duracaoMs < 50 || c.duracaoMs > 500 ||
      c.frequenciaHz < 500 || c.frequenciaHz > 4000 || c.volume > 50 ||
      c.rotacaoSeg > 30 || c.rotacaoSeg % 5 != 0) return false;
  if (c.baixaCV && c.criticaCV && c.criticaCV >= c.baixaCV) return false;
  if (c.altaCV && ((c.baixaCV && c.altaCV <= c.baixaCV) ||
                  (c.criticaCV && c.altaCV <= c.criticaCV))) return false;
  return true;
}
bool mesmosValores(const ConfigMonitor &a, const ConfigMonitor &b) {
  return a.baixaCV == b.baixaCV && a.criticaCV == b.criticaCV &&
    a.altaCV == b.altaCV && a.maximaCA == b.maximaCA && a.som == b.som &&
    a.duracaoMs == b.duracaoMs && a.frequenciaHz == b.frequenciaHz && a.volume == b.volume &&
    a.rotacaoSeg == b.rotacaoSeg && a.capacidadeAh==b.capacidadeAh &&
    a.reservaPct==b.reservaPct && a.perfilBat==b.perfilBat && a.eficienciaPct==b.eficienciaPct;
}
ConfigMonitor padroesConfig() {
  ConfigMonitor c = {};
  // Limites 0 = desativados, preservando o comportamento do codigo recebido.
  c.capacidadeAh=30; c.reservaPct=50; c.perfilBat=0; c.eficienciaPct=95;
  c.som = 1; c.duracaoMs = 150; c.frequenciaHz = 3000; c.volume = 20;
  for(uint8_t n=0;n<4;n++) c.brilhoLED[n]=100;
  return c;
}
bool lerConfig(int slot, ConfigMonitor &c) {
  File f = LittleFS.open(CONFIG_FILES[slot], "r");
  if (!f) return false;
  if (f.size() == sizeof(ConfigMonitorV1)) {
    ConfigMonitorV1 antiga = {};
    bool ok = f.read(reinterpret_cast<uint8_t *>(&antiga), sizeof(antiga)) == sizeof(antiga);
    f.close();
    if (!ok || antiga.magic != 0x4D4F4E49UL || antiga.versao != 1 ||
        antiga.checksum != checksumV1(antiga)) return false;
    c = padroesConfig();
    memcpy(&c, &antiga, offsetof(ConfigMonitorV1, checksum));
    for(uint8_t n=0;n<4;n++) c.brilhoLED[n]=100;
    c.rotacaoSeg = 0; c.checksum = checksumConfig(c);
    return validarConfig(c);
  }
  if(f.size()==sizeof(ConfigMonitorV2)) {
    ConfigMonitorV2 antiga={};
    bool ok=f.read(reinterpret_cast<uint8_t*>(&antiga),sizeof(antiga))==sizeof(antiga);
    f.close();
    if(!ok || antiga.magic!=0x4D4F4E49UL || antiga.versao!=2 || antiga.checksum!=checksumV2(antiga)) return false;
    c=padroesConfig();memcpy(&c,&antiga,offsetof(ConfigMonitorV2,checksum));
    for(uint8_t n=0;n<4;n++) c.brilhoLED[n]=100;
    c.checksum=checksumConfig(c);return validarConfig(c);
  }
  if(f.size()==sizeof(ConfigMonitorV3)) {
    ConfigMonitorV3 antiga={};
    bool ok=f.read(reinterpret_cast<uint8_t*>(&antiga),sizeof(antiga))==sizeof(antiga);
    f.close();
    const uint8_t *p=reinterpret_cast<const uint8_t*>(&antiga);uint32_t h=2166136261UL;
    for(size_t n=0;n<offsetof(ConfigMonitorV3,checksum);n++) h=(h^p[n])*16777619UL;
    if(!ok || antiga.magic!=0x4D4F4E49UL || antiga.versao!=3 || antiga.checksum!=h) return false;
    c=padroesConfig();memcpy(&c,&antiga,offsetof(ConfigMonitorV3,checksum));
    c.checksum=checksumConfig(c);return validarConfig(c);
  }
  bool ok = f.size() == sizeof(c) && f.read(reinterpret_cast<uint8_t *>(&c), sizeof(c)) == sizeof(c);
  f.close();
  return ok && c.magic == 0x4D4F4E49UL && c.versao == 4 &&
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
  if (configSalva && configuracao.versao == 4 && mesmosValores(edicao, configuracao)) return true;
  ConfigMonitor nova = edicao;
  nova.magic = 0x4D4F4E49UL; nova.versao = 4; nova.reservado = 0;
  nova.geracao = configuracao.geracao + 1;
  nova.identidadeBateria=configuracao.identidadeBateria;
  if(nova.capacidadeAh!=configuracao.capacidadeAh || nova.perfilBat!=configuracao.perfilBat)
    nova.identidadeBateria=configuracao.identidadeBateria+1;
  nova.checksum = checksumConfig(nova);
  int destino = slotConfig == 0 ? 1 : 0;
  File f = LittleFS.open(CONFIG_FILES[destino], "w");
  if (!f) return false;
  size_t escritos = f.write(reinterpret_cast<const uint8_t *>(&nova), sizeof(nova));
  f.flush(); f.close();
  ConfigMonitor conferida = {};
  if (escritos != sizeof(nova) || !lerConfig(destino, conferida) ||
      memcmp(&nova, &conferida, sizeof(nova)) != 0) return false;
  bool mudouBateria=conferida.capacidadeAh!=configuracao.capacidadeAh || conferida.perfilBat!=configuracao.perfilBat;
  configuracao = conferida; edicao = configuracao; slotConfig = destino; configSalva = true;
  if(mudouBateria) configuracaoBateriaMudou();
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
  if(itemMenu==13) return String(edicao.capacidadeAh)+" Ah";
  if(itemMenu==14) return edicao.perfilBat==0?"CHUMBO 12V":"GENERICO";
  if(itemMenu==15) return String(edicao.reservaPct)+"%";
  if(itemMenu==16) return String(edicao.eficienciaPct)+"%";
  if (itemMenu == 12) return edicao.rotacaoSeg ? String(edicao.rotacaoSeg) + " s" : "DESATIVADO";
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
  if(itemMenu==13) {edicao.capacidadeAh=constrain((int)edicao.capacidadeAh+sentido,1,1000);return;}
  if(itemMenu==14) {edicao.perfilBat=1-edicao.perfilBat;edicao.eficienciaPct=edicao.perfilBat==0?95:100;return;}
  if(itemMenu==15) {edicao.reservaPct=constrain((int)edicao.reservaPct+sentido*5,0,90);return;}
  if(itemMenu==16) {edicao.eficienciaPct=constrain((int)edicao.eficienciaPct+sentido,50,100);return;}

  if (itemMenu == 12) {
    int indice = edicao.rotacaoSeg / 5;
    edicao.rotacaoSeg = ((indice + sentido + 7) % 7) * 5;
    return;
  }
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
  telaPendente = true; cicloAlertaEm = vistaEm = millis();
  ultimaInteracaoMenu = millis();
  alertaTelaPendente = false;
  if (pagina == TELA_PRINCIPAL) {
    if (n == 1) { entrarMenuTela(); edicao = configuracao; itemMenu = 0; pagina = LISTA_MENU; }
    else navegarTela(n == 0 ? -1 : 1);
  } else if (pagina == LISTA_MENU) {
    if (n != 1) itemMenu = proximoItem(itemMenu, n == 0 ? -1 : 1);
    else if(itemMenu==17) {
      if(!mesmosValores(configuracao,edicao)) avisarMenu("Salve os ajustes antes");
      else pagina=SINCRONIZAR_MENU;
    }
    else if (itemMenu < 8 || (itemMenu >= 12 && itemMenu<=16)) pagina = EDITAR_MENU;
    else if (itemMenu == 8) {
      if (!validarConfig(edicao)) avisarMenu("Limites inconsistentes");
      else if (!salvarConfiguracao()) avisarMenu("Falha salvar LittleFS");
      else { pagina = TELA_PRINCIPAL; avisarMenu("Configuracao salva"); }
    } else if (itemMenu == 9) { edicao = configuracao; pagina = TELA_PRINCIPAL; avisarMenu("Edicao cancelada"); }
    else if (itemMenu == 10) pagina = RESTAURAR_MENU;
    else pagina = RECONHECER_MENU;
  } else if (pagina == EDITAR_MENU) {
    if (n == 1) pagina = LISTA_MENU; else ajustarItem(n == 0 ? -1 : 1);
  } else if(pagina==SINCRONIZAR_MENU) {
    if(n==1) {
      if(sincronizarBateria()) avisarMenu("Bateria em 100% salva");
      else avisarMenu("Falha: leitura ou FS");
    }
    pagina=LISTA_MENU;
  } else if (pagina == RECONHECER_MENU) {
    if (n == 1) {
      if (reconhecerAlertas()) avisarMenu("Historico reconhecido");
      else avisarMenu("Falha ao gravar ACK");
    }
    pagina = LISTA_MENU;
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
  if(pagina!=TELA_PRINCIPAL && (entradas&0x07)!=0x07) ultimaInteracaoMenu=agora;
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
  } else if(pagina==SINCRONIZAR_MENU) {
    tela.println("Confirmar carga cheia?");
    tela.println("So apos carga completa");
    tela.println("Confirmar: sim");
    tela.println("Anterior/Proximo: nao");
  } else if (pagina == RECONHECER_MENU) {
    tela.println("Reconhecer historico?");
    tela.println("Confirmar: sim");
    tela.println("Anterior/Proximo: nao");
    tela.println("Nao encerra o alarme");
  } else {
    tela.println("Restaurar padroes?"); tela.println("Confirmar: sim");
    tela.println("Anterior/Proximo: nao"); tela.println("Depois escolha Salvar");
  }
  tela.setCursor(0, 56);
  if (avisoMenu[0]) tela.print(avisoMenu);
  else if (haAlarmeAtivo()) tela.print(textoAlarmeAtivo());
  else if (pagina == EDITAR_MENU && itemMenu <= 3) tela.print("Segure Anterior: OFF");
  else tela.print("Confirmar: selecionar");
  enviarTela();
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
// Prototipo explicito porque o registrador usa obterHorario(), definido adiante.
bool registrarTransicaoAlarme(TipoAlarme anterior, TipoAlarme novo,
                              bool leituraOK, float v, float i,
                              bool reconhecimento);
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
  acionarBuzzer(true, true);
  uint32_t inicio=millis();
  while(millis()-inicio<80) {atualizarLEDs();delay(1);}
  silenciarBuzzer();
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
    notificarAlarmeTela();
    pararSom();
    telaPendente = true;
    if (!registrarTransicaoAlarme(anterior, novo, leituraOK, v, i, false)) {
      Serial.println("Falha ao registrar transicao de alarme no SD.");
    }
    somAlarmePendente = novo != SEM_ALARME;
    Serial.print("Alarme: ");
    Serial.println(novo == SEM_ALARME ? "NORMAL" : mensagemAlarme(novo));
  }
}

// Chamar DEPOIS de apresentarTela(): o aviso fica visivel durante o beep.
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
// 0: ignorar/NORMAL; 1: ALERTA; 2: ACK. Somente registros completos validos.
uint8_t tipoHistoricoLinha(char *linha) {
  char *estado = linha;
  for (uint8_t campo = 0; campo < 3; ++campo) {
    estado = strchr(estado, ',');
    if (!estado) return 0;
    ++estado;
  }
  uint8_t tipo;
  const char *campos;
  if (strncmp(estado, "ALERTA,", 7) == 0) { tipo = 1; campos = estado + 7; }
  else if (strncmp(estado, "ACK,", 4) == 0) { tipo = 2; campos = estado + 4; }
  else return 0;
  unsigned anterior, novo, leitura;
  char restante;
  // Estado, anterior, novo, leitura e tres valores; rejeita fragmentos.
  float v, i, p;
  bool valido = sscanf(campos, "%u,%u,%u,%f,%f,%f%c",
                &anterior, &novo, &leitura, &v, &i, &p, &restante) == 6 &&
         anterior <= TENSAO_ALTA && novo <= TENSAO_ALTA && leitura <= 1 &&
         (tipo == 2 || novo > SEM_ALARME) && isfinite(v) && isfinite(i) && isfinite(p);
  return valido ? tipo : 0;
}

bool aplicarHistoricoArquivo(const char *nome, bool &pendente) {
  if (!SD.exists(nome)) return true;
  File arquivo = SD.open(nome, FILE_READ);
  if (!arquivo) { logAlertasOK = false; return false; }
  // Leitura completa em ordem: um ACK pode estar fora da antiga janela
  // de 8192 bytes. Memoria fixa; yield permite atender ao ESP8266.
  char linha[192];
  size_t posicao = 0;
  bool excedeu = false, sucesso = true;
  while (arquivo.available()) {
    int c = arquivo.read();
    if (c < 0) { logAlertasOK = false; sucesso = false; break; }
    if (c == '\n') {
      linha[posicao] = '\0';
      if (!excedeu) {
        uint8_t tipo = tipoHistoricoLinha(linha);
        if (tipo == 1) pendente = true;
        else if (tipo == 2) pendente = false;
      }
      posicao = 0; excedeu = false;
    } else if (c != '\r') {
      if (posicao < sizeof(linha) - 1) linha[posicao++] = (char)c;
      else excedeu = true;
    }
    yield();
  }
  arquivo.close();
  return sucesso;
}

void carregarHistoricoAlertas() {
  historicoAlertas = false;
  if (!cartaoInicializado) return;
  // ALERTOLD e a copia anterior; ALERTAS e posterior, mesmo sem NTP.
  aplicarHistoricoArquivo(ARQUIVO_ALERTAS_ANTERIOR, historicoAlertas);
  aplicarHistoricoArquivo(ARQUIVO_ALERTAS, historicoAlertas);
  Serial.println(historicoAlertas ? "Historico: alertas ainda nao reconhecidos."
                                 : "Historico: nenhum alerta pendente de reconhecimento.");
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
                              bool leituraOK, float v, float i, bool reconhecimento) {
  if (!cartaoInicializado) { logAlertasOK = false; return false; }
  char data[32], curto[24], linha[192];
  obterHorario(data, sizeof(data), curto, sizeof(curto));
  int tamanho = snprintf(linha, sizeof(linha),
      "%s,%lu,%s,%s,%u,%u,%u,%.3f,%.6f,%.6f\n", data,
      (unsigned long)millis(), nomeArquivo[0] ? nomeArquivo + 1 : "SEM_CSV",
      reconhecimento ? "ACK" : novo == SEM_ALARME ? "NORMAL" : "ALERTA",
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
  arquivo.flush();
  arquivo.close();
  if (sucesso && reconhecimento) {
    // Confirma que o ACK completo pode ser relido antes de apagar o indicador.
    File conferir = SD.open(ARQUIVO_ALERTAS, FILE_READ);
    sucesso = conferir && conferir.size() >= (uint32_t)tamanho &&
      conferir.seek(conferir.size() - tamanho);
    for (int n = 0; sucesso && n < tamanho; n++) sucesso = conferir.read() == (uint8_t)linha[n];
    if (conferir) conferir.close();
  }
  logAlertasOK = sucesso;
  if (sucesso && reconhecimento) historicoAlertas = false;
  else if (sucesso && novo != SEM_ALARME) historicoAlertas = true;
  return sucesso;
}

bool reconhecerAlertas() {
  // Reconhece o historico, sem encerrar/silenciar o alarme atualmente ativo.
  // O proximo ALERTA registrado (inclusive reincidencia) torna a marca pendente.
  if (!cartaoInicializado) return false;
  if (!registrarTransicaoAlarme(alarmeAtual, alarmeAtual, leituraAtualOK,
                               tensaoAtual, correnteAtual, true)) return false;
  telaPendente = true;
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
      "data_hora_local,tempo_ms,tensao_V,shunt_mV,corrente_A,potencia_W,soc_pct,restante_Ah,retirados_Ah,recebidos_Ah,soc_valido,soc_retomado\n";
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
  char linha[256],soc[16]="",restante[20]="";
  float pct=percentualBateria();
  if(pct>=0) {snprintf(soc,sizeof(soc),"%.2f",pct);snprintf(restante,sizeof(restante),"%.5f",bateria.restanteAh);}
  int tamanho = snprintf(linha, sizeof(linha),
      "%s,%lu,%.3f,%.3f,%.6f,%.6f,%s,%s,%.5f,%.5f,%u,%u\n", data, (unsigned long)tempo,
      volts, milivolts, corrente, potencia,soc,restante,bateria.retiradosAh,bateria.recebidosAh,
      bateria.sincronizada?1:0,bateriaRetomada?1:0);
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

// Navegacao independente do menu; temporizadores seguros no rollover de millis.
void entrarMenuTela() {
  if (alertaForcado) vista = vistaRetorno;
  alertaForcado = false; alertaTelaPendente = false;
  cicloAlertaEm = vistaEm = millis();
}
void navegarTela(int sentido) {
  Vista anterior = vista;
  if (anterior != VISTA_ALERTA) vistaRetorno = anterior;
  vista = static_cast<Vista>((static_cast<int>(vista) + sentido + 6) % 6);
  alertaTelaPendente = false;
  alertaForcado = vista == VISTA_ALERTA && alarmeAtual != SEM_ALARME;
  cicloAlertaEm = vistaEm = millis(); telaPendente = true;
}
void notificarAlarmeTela() {
  if (alarmeAtual == SEM_ALARME) {
    alertaTelaPendente = false;
    if (alertaForcado) { vista = vistaRetorno; alertaForcado = false; }
    cicloAlertaEm = vistaEm = millis();
  } else alertaTelaPendente = true;
  telaPendente = true;
}
void forcarTelaAlerta() {
  if (vista != VISTA_ALERTA) vistaRetorno = vista;
  vista = VISTA_ALERTA; alertaForcado = true; alertaTelaPendente = false;
  vistaEm = millis(); telaPendente = true;
}
void atualizarNavegacao() {
  if (pagina != TELA_PRINCIPAL) return;
  uint32_t agora = millis();
  if (alarmeAtual != SEM_ALARME) {
    if (alertaTelaPendente) { forcarTelaAlerta(); return; }
    if (alertaForcado) {
      if (agora - vistaEm >= PERIODO_TELA_ALERTA_MS) {
        vista = vistaRetorno; alertaForcado = false;
        cicloAlertaEm = vistaEm = agora; telaPendente = true;
      }
      return;
    }
    if (agora - cicloAlertaEm >= PERIODO_TELA_ALERTA_MS) { forcarTelaAlerta(); return; }
  }
  if (!alertaForcado && configuracao.rotacaoSeg &&
      agora - vistaEm >= (uint32_t)configuracao.rotacaoSeg * 1000UL) {
    vista = vista == VISTA_ALERTA ? VISTA_RESUMO :
      static_cast<Vista>((static_cast<int>(vista) + 1) % 5);
    vistaEm = agora; telaPendente = true;
  }
}

// Somente as quatro barras de sinal. Sem simbolo de Wi-Fi redundante.
void desenharWiFi() {
  uint8_t barras = 0;
  if (WiFi.status() == WL_CONNECTED) {
    int32_t rssi = WiFi.RSSI();
    barras = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1;
  }
  for (uint8_t n = 0; n < 4; n++) {
    int16_t x = 106 + n * 5, h = 2 + n * 2;
    if (n < barras) tela.fillRect(x, 9-h, 4, h, SSD1306_WHITE);
    else tela.drawRect(x, 9-h, 4, h, SSD1306_WHITE);
  }
}
void cabecalhoTela(bool alerta) {
  tela.clearDisplay(); tela.setTextWrap(false);
  tela.setTextColor(SSD1306_WHITE); tela.setTextSize(1);
  tela.setCursor(0, 0);
  if (alerta) {
    tela.drawTriangle(4, 10, 11, 0, 18, 10, SSD1306_WHITE);
    tela.drawFastVLine(11, 3, 3, SSD1306_WHITE); tela.drawPixel(11, 8, SSD1306_WHITE);
    tela.setCursor(24, 2); tela.print("ALERTA");
  } else if (horarioAtual[0] >= '0' && horarioAtual[0] <= '9') {
    for (uint8_t n=0; n<5; n++) tela.write(horarioAtual[n]);
  } else tela.print("--/--");
  if (!alerta) {
    tela.setCursor(42, 0); tela.print(sdOK && logAlertasOK ? "SD:OK" : "SD:ERR");
  }
  if (historicoAlertas) { tela.setCursor(94, 0); tela.print('!'); }
  desenharWiFi(); tela.drawFastHLine(4, 12, 120, SSD1306_WHITE);
}
int nivelBateria(float v, bool valida) {
  float pct=percentualBateria();
  if(!valida || v<2 || pct<0) return -1;
  if(pct<=0) return 0;
  return pct>75?4:pct>50?3:pct>25?2:1;
}
void desenharBateria(int nivel) {
  tela.drawRoundRect(8, 24, 24, 33, 4, SSD1306_WHITE);
  tela.fillRoundRect(15, 20, 10, 5, 2, SSD1306_WHITE);
  if (nivel < 0) { tela.setCursor(17, 37); tela.print('?'); return; }
  bool piscar = (millis()/600UL) % 2 == 0;
  if (nivel == 0) {
    if (piscar) { tela.setCursor(17, 37); tela.print('!'); }
  } else {
    for (int n=0; n<nivel; n++) {
      if (nivel == 1 && !piscar) continue;
      tela.fillRect(12, 49-n*7, 16, 5, SSD1306_WHITE);
    }
  }
}
void desenharTomada(bool consumo) {
  // Cabo curvo e plugue, inspirados na referencia enviada.
  tela.drawRoundRect(5, 22, 17, 14, 6, SSD1306_WHITE);
  tela.fillRect(4, 29, 8, 9, SSD1306_BLACK);
  tela.drawFastVLine(7, 32, 11, SSD1306_WHITE);
  tela.drawFastHLine(7, 43, 9, SSD1306_WHITE);
  tela.fillRoundRect(14, 38, 14, 17, 4, SSD1306_WHITE);
  tela.fillRect(27, 41, 7, 3, SSD1306_WHITE);
  tela.fillRect(27, 49, 7, 3, SSD1306_WHITE);
  if (consumo) tela.fillCircle(6+(millis()/200UL)%4*5, 59, 1, SSD1306_WHITE);
}
void desenharRaio(bool consumo) {
  bool cheio = !consumo || (millis()/500UL)%2 == 0;
  if (cheio) {
    tela.fillTriangle(26, 20, 8, 42, 21, 42, SSD1306_WHITE);
    tela.fillTriangle(16, 37, 31, 37, 12, 58, SSD1306_WHITE);
  } else {
    tela.drawTriangle(26, 20, 8, 42, 21, 42, SSD1306_WHITE);
    tela.drawTriangle(16, 37, 31, 37, 12, 58, SSD1306_WHITE);
  }
}
void valorGrandeDireita(float valor, bool valida) {
  char texto[24];
  if (valida && isfinite(valor)) snprintf(texto, sizeof(texto), "%.3f", valor);
  else snprintf(texto, sizeof(texto), "---");
  uint8_t escala = strlen(texto)*12 <= 84 ? 2 : 1;
  tela.setTextSize(escala);
  tela.setCursor(124-strlen(texto)*6*escala, 29+(escala==1?4:0)); tela.print(texto);
  tela.setTextSize(1);
}
void desenharTelaAlerta() {
  cabecalhoTela(true);
  const char *a="SEM", *b="ALERTAS";
  switch(alarmeAtual) {
    case BATERIA_BAIXA: a="BATERIA";b="BAIXA";break;
    case BATERIA_CRITICA: a="BATERIA";b="CRITICA";break;
    case TENSAO_ALTA: a="TENSAO";b="ALTA";break;
    case SOBRECORRENTE: a="SOBRE";b="CORRENTE";break;
    case ERRO_SD: a="ERRO NO";b="CARTAO SD";break;
    case FALHA_INA226: a="FALHA NO";b="INA226";break;
    default: if(historicoAlertas){a="HISTORICO";b="PENDENTE";} break;
  }
  tela.setTextSize(2); tela.setCursor(4, 20);tela.print(a);
  tela.setCursor(4, 39);tela.print(b);tela.setTextSize(1);
  tela.setCursor(4, 56);
  tela.print(alarmeAtual == SEM_ALARME ? "Confirmar: menu" : "Proximo: sair");
  enviarTela();
}
void desenharTelaBateria() {
  cabecalhoTela(false);
  float pct=percentualBateria();
  desenharBateria(nivelBateria(tensaoAtual,leituraAtualOK));
  tela.setCursor(40,16);tela.print("BATERIA ");tela.print(configuracao.capacidadeAh);tela.print("Ah");
  tela.setCursor(40,27);tela.setTextSize(2);
  if(pct<0) tela.print("--%");else {tela.print(pct,0);tela.print('%');}
  tela.setTextSize(1);tela.setCursor(40,45);
  if(pct<0) {tela.print("Saiu:");tela.print(bateria.retiradosAh,2);tela.print("Ah");}
  else {tela.print(bateria.restanteAh,1);tela.print("Ah ");tela.print(bateriaRetomada?"~":"EST.");}
  char autonomia[24];textoAutonomia(autonomia,sizeof(autonomia));
  tela.setCursor(4,56);
  if(!bateriaPersistenciaOK) tela.print("BAT: ERRO AO SALVAR");
  else if(pct<0) tela.print("Menu: Bateria cheia");
  else {
    if((autonomia[0]>='0' && autonomia[0]<='9') || autonomia[0]=='>') {
      tela.print("Ate ");tela.print(configuracao.reservaPct);tela.print("% ");
    }
    tela.print(autonomia);
  }
  enviarTela();
}
void desenharTelaOperacao() {
  if (!oledOK) return;
  if (vista == VISTA_ALERTA) { desenharTelaAlerta(); return; }
  if (vista == VISTA_BATERIA) { desenharTelaBateria(); return; }
  cabecalhoTela(false);
  if (vista == VISTA_RESUMO) {
    desenharMedicao('V', tensaoAtual, 16, leituraAtualOK);
    desenharMedicao('A', correnteAtual, 32, leituraAtualOK);
    desenharMedicao('W', potenciaAtual, 48, leituraAtualOK);
  } else {
    tela.setCursor(44, 16);
    if (vista == VISTA_TENSAO) {
      tela.print("TENSAO"); int n=nivelBateria(tensaoAtual, leituraAtualOK);
      desenharBateria(n); valorGrandeDireita(tensaoAtual, leituraAtualOK);
      tela.setCursor(44, 49);tela.print("V ");
      float pct=percentualBateria();
      if(pct<0) tela.print("SOC: --");else {tela.print(pct,0);tela.print("% EST.");}
      tela.setCursor(44, 57);tela.print(pct<0?"SINCRONIZAR":"EST. POR Ah");
    } else if (vista == VISTA_CORRENTE) {
      tela.print("CORRENTE"); desenharTomada(leituraAtualOK && fabsf(correnteAtual)>0.05f);
      valorGrandeDireita(correnteAtual, leituraAtualOK);
      tela.setCursor(44, 49);tela.print("A");
    } else {
      tela.print("POTENCIA");desenharRaio(leituraAtualOK && fabsf(potenciaAtual)>0.10f);
      valorGrandeDireita(potenciaAtual, leituraAtualOK);
      tela.setCursor(44, 49);tela.print("W");
    }
  }
  enviarTela();
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
  enviarTela();
  beepInicializacao();
  uint32_t inicioLogo=millis();
  while(millis()-inicioLogo<3000) {atualizarLEDs();delay(1);}
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

void setup() {
  digitalWrite(BUZZER, NIVEL_BUZZER_DESLIGADO);
  pinMode(BUZZER, OUTPUT);
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== MONITOR: BATERIA / Ah ===");
  Serial.print("Motivo do reset: ");Serial.println(ESP.getResetReason());
  Serial.println(ESP.getResetInfo());
  Wire.begin(D2, D1);
  carregarConfiguracao();
  carregarBateria();
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
  Serial.println("Mostrando logo de inicializacao...");
  if (oledOK) mostrarLogo();
  else beepInicializacao();
  vistaEm = cicloAlertaEm = millis();
  desenharTelaOperacao();
  Serial.println("Inicializacao concluida; iniciando medicoes.");
  ultimaMedicao = millis();
}

// Vermelho: alarme/falha local; amarelo: historico ou edicao pendente;
// verde: leitura valida sem alarme; azul: Wi-Fi conectado.
void atualizarLEDs() {
  if (!mcpOK || falhaMCP) return;
  uint8_t leds = 0;
  if (alarmeAtual != SEM_ALARME || !fsOK || !bateriaPersistenciaOK) leds |= 0x01;
  if (historicoAlertas || !mesmosValores(configuracao, edicao)) leds |= 0x02;
  if (leituraAtualOK && alarmeAtual == SEM_ALARME) leds |= 0x04;
  if (WiFi.status() == WL_CONNECTED) leds |= 0x08;
  static uint8_t anterior = 0xFF;
  if (leds != anterior && escreverMCP(0x15, leds)) anterior = leds;
}
void verificarTempoMenu() {
  if(pagina==TELA_PRINCIPAL || millis()-ultimaInteracaoMenu<INATIVIDADE_MENU_MS) return;
  edicao=configuracao;pagina=TELA_PRINCIPAL;avisoMenu="";
  vista=VISTA_RESUMO;alertaForcado=false;
  vistaEm=cicloAlertaEm=millis();telaPendente=true;
  // Um alarme surgido durante o menu conserva sua prioridade ao retornar.
  Serial.println("Menu: retorno por inatividade; alteracoes pendentes descartadas.");
}
// Envio horizontal do framebuffer em blocos curtos, cedendo execucao ao SDK.
void enviarTela() {
  if(!oledOK) return;
  Wire.setClock(400000);
  Wire.beginTransmission((uint8_t)0x3C);Wire.write((uint8_t)0x00);
  const uint8_t comandos[]={SSD1306_MEMORYMODE,0x00,SSD1306_PAGEADDR,0,7,SSD1306_COLUMNADDR,0,127};
  for(uint8_t c:comandos) Wire.write(c);
  bool ok=Wire.endTransmission()==0;
  uint8_t *buffer=tela.getBuffer();
  for(uint16_t pos=0;ok && pos<1024;pos+=16) {
    Wire.beginTransmission((uint8_t)0x3C);Wire.write((uint8_t)0x40);
    Wire.write(buffer+pos,(size_t)16);ok=Wire.endTransmission()==0;
    if((pos%128)==112) yield();
  }
  Wire.setClock(100000);
}
void apresentarTela() {
  // O alerta nao retira o usuario do menu. No menu aparece ! e o texto
  // no rodape; som, medicoes e registros continuam ativos.
  if (pagina == TELA_PRINCIPAL)
    desenharTelaOperacao();
  else desenharMenu();
  telaPendente = false;
}
void loop() {
  atualizarLEDs();verificarWiFi();
  static uint32_t ultimaConsultaBotoes=0;
  if(millis()-ultimaConsultaBotoes>=5) {ultimaConsultaBotoes=millis();verificarBotoes();}
  verificarTempoMenu();atualizarSom();
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
      atualizarContagem(true,tensaoAtual,correnteAtual,agora);
      if (sdOK && !gravarMedicao(data, agora, tensaoAtual, voltsShunt * 1000.0f,
                                correnteAtual, potenciaAtual)) {
        sdOK = false; Serial.println("Falha SD: gravacao interrompida. Verifique o cartao.");
      }
      Serial.printf("%s | Tensao: %.3f V | Corrente: %.3f A | Potencia: %.3f W | SD: %s | WiFi: %s | FS: %s\n",
        data, tensaoAtual, correnteAtual, potenciaAtual,
        sdOK && logAlertasOK ? "OK" : "ERRO", WiFi.status() == WL_CONNECTED ? "OK" : "DESCONECTADO",
        fsOK ? "OK" : "ERRO");
    }
    if(!leituraAtualOK) atualizarContagem(false,0,0,agora);
    char autonomia[24];textoAutonomia(autonomia,sizeof(autonomia));
    if(percentualBateria()<0) Serial.printf("Bateria: SOC desconhecido | Retirados: %.4f Ah | Recebidos: %.4f Ah\n",bateria.retiradosAh,bateria.recebidosAh);
    else Serial.printf("Bateria: %.1f%% EST%s | Restante: %.3f Ah | Autonomia: %s | BAT_FS: %s\n",percentualBateria(),bateriaRetomada?" RETOMADA":"",bateria.restanteAh,autonomia,bateriaPersistenciaOK?"OK":"ERRO");
    avaliarAlarme(leituraAtualOK, tensaoAtual, correnteAtual);
    telaPendente = true;
  }
  manterBateria();
  atualizarNavegacao();
  if (telaPendente || agora - ultimaTela >= 200) {
    apresentarTela(); ultimaTela = agora;
  }
  emitirAlarmeSeNecessario(); atualizarLEDs();
  // Pausa real periodica: delay(0) sozinho nao garante atendimento suficiente
  // ao SDK/watchdog durante loops apertados com trafego I2C.
  static uint32_t ultimaPausaSDK=0;
  if(millis()-ultimaPausaSDK>=20) {delay(1);ultimaPausaSDK=millis();}
  else yield();
}