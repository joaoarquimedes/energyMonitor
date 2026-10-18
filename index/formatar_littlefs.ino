/* FORMATACAO AUTORIZADA DO LITTLEFS - NodeMCU ESP8266 / core 3.1.2
   Apaga TODOS os arquivos LittleFS; nao acessa nem formata o microSD.
   IDE: NodeMCU 1.0 (ESP-12E Module)
   Flash Size: 4MB (FS:1MB OTA:~1019KB). Erase Flash: Only Sketch.
   Serial: 115200. Envie FORMATAR_LITTLEFS para executar uma vez.
   A confirmacao serial evita apagar novamente a cada reset.
   Apos sucesso, substitua este sketch pelo firmware do monitor. */
#include <Arduino.h>
#include <LittleFS.h>
#include <flash_hal.h>
#include <string.h>

bool tentou = false;
char comando[40] = {};
uint8_t posicao = 0;
bool linhaLonga = false;

void executarFormatacao() {
  if (tentou) {
    Serial.println("Operacao ja executada/tentada neste boot. Nao sera repetida.");
    return;
  }
  tentou = true;
  uint32_t real = ESP.getFlashChipRealSize();
  uint32_t configurada = ESP.getFlashChipSize();
  uint32_t inicio = FS_PHYS_ADDR, tamanho = FS_PHYS_SIZE;
  Serial.printf("Flash fisica/configurada: %lu / %lu bytes\n", (unsigned long)real, (unsigned long)configurada);
  Serial.printf("LittleFS: inicio=0x%08lX tamanho=%lu bytes\n", (unsigned long)inicio, (unsigned long)tamanho);
  // Limites confirmados no diagnostico do usuario para FS:1MB/core 3.1.2.
  if (real != 4194304UL || configurada != real || inicio != 0x00300000UL ||
      tamanho != 1024000UL || inicio > real || tamanho > real - inicio) {
    Serial.println("ABORTADO: layout diferente do diagnosticado.");
    Serial.println("Selecione 4MB (FS:1MB OTA:~1019KB) e envie novamente.");
    return;
  }
  LittleFSConfig cfg;
  cfg.setAutoFormat(false);
  LittleFS.setConfig(cfg);
  LittleFS.end();
  Serial.println("[1] Formatando SOMENTE o LittleFS. Aguarde...");
  uint32_t inicioMs = millis();
  if (!LittleFS.format()) {
    Serial.println("FALHOU: LittleFS.format(). Envie o log completo.");
    return;
  }
  Serial.printf("Formatacao OK (%lu ms).\n", (unsigned long)(millis() - inicioMs));
  Serial.println("[2] Montando o volume criado...");
  if (!LittleFS.begin()) {
    Serial.println("FALHOU: formatou, mas nao montou. Envie o log completo.");
    return;
  }
  FSInfo info;
  if (LittleFS.info(info))
    Serial.printf("Total: %lu bytes; usados: %lu bytes.\n", (unsigned long)info.totalBytes, (unsigned long)info.usedBytes);

  Serial.println("[3] Testando gravacao, fechamento e releitura...");
  const char *nome = "/teste_lfs.txt";
  const char texto[] = "LittleFS: gravacao e leitura confirmadas.\n";
  File f = LittleFS.open(nome, "w");
  if (!f) { Serial.println("FALHOU: abrir para escrita."); LittleFS.end(); return; }
  size_t escritos = f.write(reinterpret_cast<const uint8_t *>(texto), sizeof(texto) - 1);
  f.flush(); f.close();
  if (escritos != sizeof(texto) - 1) {
    Serial.println("FALHOU: escrita incompleta."); LittleFS.end(); return;
  }
  // Remontar antes da leitura confirma a recuperacao do arquivo pela biblioteca.
  LittleFS.end();
  if (!LittleFS.begin()) { Serial.println("FALHOU: remontagem apos escrita."); return; }
  f = LittleFS.open(nome, "r");
  if (!f) { Serial.println("FALHOU: abrir para leitura."); LittleFS.end(); return; }
  char recebido[sizeof(texto)] = {};
  size_t tamanhoArquivo = f.size();
  size_t lidos = f.read(reinterpret_cast<uint8_t *>(recebido), sizeof(texto) - 1);
  f.close();
  bool ok = tamanhoArquivo == sizeof(texto) - 1 && lidos == sizeof(texto) - 1 &&
    memcmp(texto, recebido, sizeof(texto) - 1) == 0;
  if (!ok) { Serial.println("FALHOU: conteudo/tamanho diferente do gravado."); LittleFS.end(); return; }
  Serial.println("Gravacao, remontagem e leitura: OK.");
  if (!LittleFS.remove(nome)) {
    Serial.println("FALHOU: exclusao do arquivo de teste."); LittleFS.end(); return;
  }
  LittleFS.end();
  Serial.println("=== SUCESSO: LITTLEFS FORMATADO E TESTADO ===");
  Serial.println("Reenvie monitor_bateria.ino com o MESMO FS:1MB e Erase Flash: Only Sketch.");
  Serial.println("Os ajustes e o SOC comecarao nos padroes/desconhecido. O SD nao foi acessado.");
}

void setup() {
  digitalWrite(D0, LOW); pinMode(D0, OUTPUT); // Buzzer atual HIGH: desligado no sketch.
  digitalWrite(D4, HIGH); pinMode(D4, OUTPUT); // SD desmarcado, sem iniciar SPI.
  Serial.begin(115200);
  delay(1200);
  Serial.println("\n=== FORMATADOR LITTLEFS - SEM ACESSO AO SD ===");
  Serial.println("Apaga configuracoes e saldo da bateria na flash interna.");
  Serial.println("Nao formata automaticamente ao reiniciar.");
  Serial.println("Envie FORMATAR_LITTLEFS e Enter para executar.");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (linhaLonga) Serial.println("Comando longo demais; ignorado.");
      else if (posicao) {
        comando[posicao] = '\0';
        if (strcmp(comando, "FORMATAR_LITTLEFS") == 0) executarFormatacao();
        else Serial.println("Comando nao reconhecido. Use FORMATAR_LITTLEFS.");
      }
      posicao = 0; linhaLonga = false;
    } else if (posicao < sizeof(comando) - 1 && !linhaLonga) comando[posicao++] = c;
    else linhaLonga = true;
  }
  delay(10);
}
