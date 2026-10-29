#include <Arduino.h>
#include "arduino_freertos.h"
#include <semphr.h>
#include <SD.h>
#include <SPI.h>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#if defined(ARDUINO_TEENSY41)
#include <QNEthernet.h>
namespace qn = qindesign::network;
#endif

namespace {
constexpr uint8_t kConsolePriority = 2;
constexpr uint8_t kClockPriority = 1;
constexpr uint32_t kConsoleStackWords = 1024;
constexpr uint32_t kClockStackWords = 192;
constexpr uint32_t kStartupStackWords = 512;
constexpr size_t kLineCapacity = 128;
constexpr size_t kPathCapacity = 160;
#if defined(ARDUINO_TEENSY41)
constexpr bool kHasBuiltInSdSocket = true;
#else
constexpr bool kHasBuiltInSdSocket = false;
#endif
constexpr uint8_t kDefaultSdChipSelect = 10;  // SPI fallback for Teensy 4.0.

struct RuntimeState {
  uint32_t uptime_seconds = 0;
  uint8_t sd_chip_select = kDefaultSdChipSelect;
  bool sd_mounted = false;
  bool echo_enabled = true;
  bool ethernet_initialized = false;
  bool ethernet_link = false;
  bool ethernet_has_ip = false;
  bool tcp_listening = false;
  uint8_t ethernet_ip[4]{};
};

RuntimeState state;
char current_directory[kPathCapacity] = "/";
SemaphoreHandle_t shellMutex = nullptr;

void consolePrintf(Stream& output, const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (length > 0) output.print(buffer);
}

RuntimeState snapshot() {
  RuntimeState copy;
  taskENTER_CRITICAL();
  copy = state;
  taskEXIT_CRITICAL();
  return copy;
}

bool equalsIgnoreCase(const char* a, const char* b) {
  while (*a && *b) {
    char ca = *a++, cb = *b++;
    if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
    if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
    if (ca != cb) return false;
  }
  return *a == '\0' && *b == '\0';
}

void printPrompt(Stream& output) {
  output.print(current_directory);
  output.print((" > "));
}

bool resolvePath(const char* input, char* output, size_t output_size) {
  if (!input || !*input || !output || output_size < 2) return false;
  char combined[kPathCapacity * 2]{};
  if (input[0] == '/') {
    snprintf(combined, sizeof(combined), "%s", input);
  } else if (strcmp(current_directory, "/") == 0) {
    snprintf(combined, sizeof(combined), "/%s", input);
  } else {
    snprintf(combined, sizeof(combined), "%s/%s", current_directory, input);
  }
  if (strlen(combined) >= sizeof(combined) - 1) return false;

  char normalized[kPathCapacity] = "/";
  char* save = nullptr;
  for (char* part = strtok_r(combined, "/", &save); part; part = strtok_r(nullptr, "/", &save)) {
    if (strcmp(part, ".") == 0 || !*part) continue;
    if (strcmp(part, "..") == 0) {
      if (strcmp(normalized, "/") != 0) {
        char* slash = strrchr(normalized, '/');
        if (slash == normalized) normalized[1] = '\0';
        else if (slash) *slash = '\0';
      }
      continue;
    }
    const size_t used = strlen(normalized);
    const int written = snprintf(normalized + used, sizeof(normalized) - used,
                                 "%s%s", used == 1 ? "" : "/", part);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(normalized) - used) return false;
  }
  if (strlen(normalized) + 1 > output_size) return false;
  strcpy(output, normalized);
  return true;
}

bool requireMounted(Stream& output) {
  if (snapshot().sd_mounted) return true;
  output.println(("Nincs felcsatolt SD. Parancs: sd mount [CS-lab]"));
  return false;
}

void printHelp(Stream& output) {
  output.println(("Rendszer:"));
  output.println(("  help | ?             parancsok"));
  output.println(("  status               RTOS, memoria, uzemido"));
  output.println(("  devices              ismert eszkozok"));
#if defined(ARDUINO_TEENSY41)
  output.println(("  net                  Ethernet/DHCP/TCP allapot"));
#endif
  output.println(("  echo on|off          helyi visszhang"));
  output.println(("SD/fajlrendszer:"));
  if (kHasBuiltInSdSocket)
    output.println("  sd mount             beépített SDIO microSD csatolása (FAT/exFAT)");
  else
    output.println("  sd mount [CS-lab]    SPI SD csatolasa (alapertelmezett CS=10)");
  output.println(("  pwd | cd <utvonal>   helyi konyvtar"));
  output.println(("  ls [utvonal]         konyvtar tartalma"));
  output.println(("  mkdir <utvonal>      konyvtar letrehozasa"));
  output.println(("  rmdir <utvonal>      ures konyvtar torlese"));
  output.println(("  cat <fajl>           fajl kiirasa"));
  output.println(("  put <fajl>           feluliras sorokkal; egy pont zarja le"));
  output.println(("  rm <fajl>            fajl torlese"));
}

void commandStatus(Stream& output) {
  const RuntimeState s = snapshot();
  consolePrintf(output, "RTOS: FreeRTOS %s, fut; uptime=%lu s; taskok=%u\r\n",
                tskKERNEL_VERSION_NUMBER,
                static_cast<unsigned long>(s.uptime_seconds),
                static_cast<unsigned>(uxTaskGetNumberOfTasks()));
  const auto ram1 = freertos::ram1_usage();
  const auto ram2 = freertos::ram2_usage();
  const auto heap = freertos::heap_usage();
  const size_t allocator_free = xPortGetFreeHeapSize();
  const size_t total_heap_free = std::get<1>(heap) + allocator_free;
  consolePrintf(output, "RAM1/DTCM: szabad=%lu B; statikus adat=%lu B; BSS=%lu B\r\n",
                static_cast<unsigned long>(std::get<0>(ram1)),
                static_cast<unsigned long>(std::get<1>(ram1)),
                static_cast<unsigned long>(std::get<2>(ram1)));
  consolePrintf(output, "RAM2: szabad=%lu / %lu B\r\n",
                static_cast<unsigned long>(std::get<0>(ram2)),
                static_cast<unsigned long>(std::get<1>(ram2)));
  consolePrintf(output, "Heap: foglalt=%lu B; allocatorban szabad=%lu B; meg kioszthato=%lu B; osszes szabad=%lu B\r\n",
                static_cast<unsigned long>(std::get<0>(heap)),
                static_cast<unsigned long>(allocator_free),
                static_cast<unsigned long>(std::get<1>(heap)),
                static_cast<unsigned long>(total_heap_free));
  if (kHasBuiltInSdSocket)
    consolePrintf(output, "SD: %s; beépített SDIO foglalat\r\n", s.sd_mounted ? "csatolva" : "nincs csatolva");
  else
    consolePrintf(output, "SD: %s; SPI CS=%u\r\n", s.sd_mounted ? "csatolva" : "nincs csatolva",
                  static_cast<unsigned>(s.sd_chip_select));
  consolePrintf(output, "Ethernet: %s; link=%s; IP=%u.%u.%u.%u; TCP/2323=%s\r\n",
                s.ethernet_initialized ? "inditva" : "nem indult",
                s.ethernet_link ? "up" : "down", s.ethernet_ip[0], s.ethernet_ip[1],
                s.ethernet_ip[2], s.ethernet_ip[3], s.tcp_listening ? "figyel" : "nem figyel");
}

void commandDevices(Stream& output) {
  const RuntimeState s = snapshot();
  output.println("USB / rendszer:");
  output.println("  usb0     USB CDC konzol                 hasznalatban");
  output.println("  rtos0    FreeRTOS kernel                fut");
  if (kHasBuiltInSdSocket)
    consolePrintf(output, "  sd0      SDIO microSD                  %s\r\n", s.sd_mounted ? "csatolva" : "nincs csatolva");
  else
    consolePrintf(output, "  sd0      SPI SD/FAT                    %s (CS=%u)\r\n",
                  s.sd_mounted ? "csatolva" : "nincs csatolva",
                  static_cast<unsigned>(s.sd_chip_select));
#if defined(ARDUINO_TEENSY41)
  consolePrintf(output, "  net0     Ethernet/lwIP                 %s; TCP 2323\r\n",
                s.ethernet_initialized ? "inditva" : "nem indult");
#else
  output.println("  net0     Ethernet                      nincs ezen a modellen");
#endif

  output.println("UART (3.3 V TTL; most csak az USB CDC van megnyitva):");
#if defined(ARDUINO_TEENSY41)
  output.println("  tty1     Serial1  RX=0       TX=1       elerheto, zarva");
  output.println("  tty2     Serial2  RX=7       TX=8       elerheto, zarva");
  output.println("  tty3     Serial3  RX=15      TX=14      elerheto, zarva");
  output.println("  tty4     Serial4  RX=16      TX=17      elerheto, zarva");
  output.println("  tty5     Serial5  RX=21/46   TX=20/47   elerheto, zarva");
  output.println("  tty6     Serial6  RX=25      TX=24      elerheto, zarva");
  output.println("  tty7     Serial7  RX=28      TX=29      elerheto, zarva");
  output.println("  tty8     Serial8  RX=34/48   TX=35      elerheto, zarva");
#elif defined(ARDUINO_TEENSY40)
  output.println("  tty1     Serial1  RX=0       TX=1       elerheto, zarva");
  output.println("  tty2     Serial2  RX=7       TX=8       elerheto, zarva");
  output.println("  tty3     Serial3  RX=15      TX=14      elerheto, zarva");
  output.println("  tty4     Serial4  RX=16      TX=17      elerheto, zarva");
  output.println("  tty5     Serial5  RX=21/38   TX=20/39   elerheto, zarva");
  output.println("  tty6     Serial6  RX=25      TX=24      elerheto, zarva");
  output.println("  tty7     Serial7  RX=28      TX=29      elerheto, zarva");
#endif

  output.println("SPI master buszok (nincs megnyitva; CS kulon GPIO lehet):");
  output.println("  spi0     SCK=13    MOSI=11   MISO=12    CS=10 alap");
#if defined(ARDUINO_TEENSY41)
  output.println("  spi1     SCK=27    MOSI=26   MISO=1     CS=0 alap");
  output.println("  spi2     SCK=45    MOSI=43   MISO=42    CS=44 alap; SDIO-labakkal kozos");
#elif defined(ARDUINO_TEENSY40)
  output.println("  spi1     SCK=27    MOSI=26   MISO=1     CS=0 alap");
  output.println("  spi2     SCK=37    MOSI=35   MISO=34    CS=36 alap; also padok");
#endif
  output.println("Megjegyzes: a digitalis I/O 3.3 V-os, nem 5 V tolerans.");
  output.println("  gpio     GPIO-labak; jelenleg nincs shell-illeszto");
}

void commandSd(Stream& output, char* arg, char* extra) {
  RuntimeState s = snapshot();
  if (!arg) {
    if (kHasBuiltInSdSocket)
      consolePrintf(output, "SD: %s, beépített SDIO foglalat\r\n", s.sd_mounted ? "csatolva" : "nincs csatolva");
    else
      consolePrintf(output, "SD: %s, SPI CS=%u\r\n", s.sd_mounted ? "csatolva" : "nincs csatolva",
                    static_cast<unsigned>(s.sd_chip_select));
    return;
  }
  if (!equalsIgnoreCase(arg, "mount")) {
    output.println(("Hasznalat: sd [mount [CS-lab]]"));
    return;
  }
  uint8_t cs = s.sd_chip_select;
  if (kHasBuiltInSdSocket && extra) {
    output.println("A Teensy 4.1 beépített SDIO foglalata nem kér CS-lábat.");
    return;
  }
  if (extra) {
    char* end = nullptr;
    const unsigned long requested = strtoul(extra, &end, 10);
    if (*extra == '\0' || *end != '\0' || requested > 39) {
      output.println("Ervenytelen CS-lab (Teensy 4.0: 0..39)");
      return;
    }
    cs = static_cast<uint8_t>(requested);
  }
  bool ok = false;
  if (kHasBuiltInSdSocket) {
    output.print(("SDIO microSD foglalat csatolasa ... "));
    ok = SD.begin(BUILTIN_SDCARD);
  } else {
    consolePrintf(output, "SD csatolasa SPI-n, CS=%u ... ", static_cast<unsigned>(cs));
    ok = SD.begin(cs);
  }
  taskENTER_CRITICAL();
  state.sd_chip_select = cs;
  state.sd_mounted = ok;
  taskEXIT_CRITICAL();
  output.println(ok ? ("OK") : ("sikertelen; ellenorizd a kartyat, adaptert es CS-labat"));
}

void commandList(Stream& output, char* arg) {
  if (!requireMounted(output)) return;
  char path[kPathCapacity];
  if (!arg) strcpy(path, current_directory);
  else if (!resolvePath(arg, path, sizeof(path))) { output.println(("Tul hosszu vagy hibas utvonal")); return; }
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) { output.println(("Nem talalhato konyvtar")); if (dir) dir.close(); return; }
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    output.print(entry.isDirectory() ? ("<DIR>  ") : ("        "));
    output.print(entry.name());
    if (!entry.isDirectory()) {
      output.print(("  "));
      output.print(static_cast<unsigned long>(entry.size()));
      output.print((" B"));
    }
    output.println();
    entry.close();
  }
  dir.close();
}

void commandCd(Stream& output, char* arg) {
  if (!arg) { output.println(("Hasznalat: cd <utvonal>")); return; }
  if (!requireMounted(output)) return;
  char path[kPathCapacity];
  if (!resolvePath(arg, path, sizeof(path))) { output.println(("Hibas vagy tul hosszu utvonal")); return; }
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) { output.println(("Nem talalhato konyvtar")); if (dir) dir.close(); return; }
  dir.close();
  strcpy(current_directory, path);
}

void commandMkdir(Stream& output, char* arg, bool remove_directory) {
  if (!arg) { output.println(remove_directory ? ("Hasznalat: rmdir <utvonal>") : ("Hasznalat: mkdir <utvonal>")); return; }
  if (!requireMounted(output)) return;
  char path[kPathCapacity];
  if (!resolvePath(arg, path, sizeof(path))) { output.println(("Hibas vagy tul hosszu utvonal")); return; }
  if (strcmp(path, "/") == 0) { output.println(("A gyoker nem modositthato")); return; }
  const bool ok = remove_directory ? SD.rmdir(path) : SD.mkdir(path);
  output.println(ok ? ("OK") : ("Sikertelen; rmdir csak ures konyvtarra"));
}

void commandCat(Stream& output, char* arg) {
  if (!arg) { output.println(("Hasznalat: cat <fajl>")); return; }
  if (!requireMounted(output)) return;
  char path[kPathCapacity];
  if (!resolvePath(arg, path, sizeof(path))) { output.println(("Hibas vagy tul hosszu utvonal")); return; }
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) { output.println(("Nem talalhato fajl")); if (file) file.close(); return; }
  while (file.available()) output.write(static_cast<uint8_t>(file.read()));
  file.close();
}

bool readConsoleLine(Stream& input, Stream& output, char* buffer, size_t size, bool show_prompt,
                     bool service_network) {
  size_t used = 0;
  bool overflow = false;
  if (show_prompt) output.print(("... "));
  for (;;) {
    while (input.available() > 0) {
      const char c = static_cast<char>(input.read());
      if (c == '\r' || c == '\n') {
        if (used || overflow) {
          buffer[used] = '\0';
          if (snapshot().echo_enabled) output.println();
          return !overflow;
        }
      } else if (c == '\b' || c == 127) {
        if (used) { --used; if (snapshot().echo_enabled) output.print(("\b \b")); }
      } else if (c >= 32 && c <= 126) {
        if (used + 1 < size) {
          buffer[used++] = c;
          if (snapshot().echo_enabled) output.write(c);
        } else overflow = true;
      }
    }
    if (service_network) {
#if defined(ARDUINO_TEENSY41)
      qn::Ethernet.loop();
#endif
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void commandPut(Stream& input, Stream& output, char* arg, bool service_network) {
  if (!arg) { output.println(("Hasznalat: put <fajl>; a vege egyetlen pont")); return; }
  if (!requireMounted(output)) return;
  char path[kPathCapacity];
  if (!resolvePath(arg, path, sizeof(path))) { output.println(("Hibas vagy tul hosszu utvonal")); return; }
  if (SD.exists(path) && !SD.remove(path)) { output.println(("A regi fajl nem torolheto")); return; }
  File file = SD.open(path, FILE_WRITE);
  if (!file) { output.println(("Nem sikerult letrehozni a fajlt")); return; }
  output.println(("Szoveg bevitele; egyetlen '.' zarja le:"));
  char line[kLineCapacity];
  while (readConsoleLine(input, output, line, sizeof(line), true, service_network)) {
    if (strcmp(line, ".") == 0) break;
    file.print(line);
    file.write('\r');
    file.write('\n');
  }
  file.flush();
  file.close();
  output.println(("Fajl elmentve"));
}

void commandRemove(Stream& output, char* arg) {
  if (!arg) { output.println(("Hasznalat: rm <fajl>")); return; }
  if (!requireMounted(output)) return;
  char path[kPathCapacity];
  if (!resolvePath(arg, path, sizeof(path))) { output.println(("Hibas vagy tul hosszu utvonal")); return; }
  if (strcmp(path, "/") == 0) { output.println(("A gyoker nem torolheto")); return; }
  output.println(SD.remove(path) ? ("OK") : ("Sikertelen"));
}

void processCommand(Stream& input, Stream& output, char* line, bool service_network) {
  char* save = nullptr;
  char* command = strtok_r(line, " \t", &save);
  if (!command) return;
  char* arg = strtok_r(nullptr, " \t", &save);
  char* extra = strtok_r(nullptr, " \t", &save);

  if (shellMutex) xSemaphoreTake(shellMutex, portMAX_DELAY);
  if (equalsIgnoreCase(command, "?") || equalsIgnoreCase(command, "help")) printHelp(output);
  else if (equalsIgnoreCase(command, "status")) commandStatus(output);
  else if (equalsIgnoreCase(command, "devices")) commandDevices(output);
  else if (equalsIgnoreCase(command, "net")) commandStatus(output);
  else if (equalsIgnoreCase(command, "sd")) commandSd(output, arg, extra);
  else if (equalsIgnoreCase(command, "pwd")) output.println(current_directory);
  else if (equalsIgnoreCase(command, "ls")) commandList(output, arg);
  else if (equalsIgnoreCase(command, "cd")) commandCd(output, arg);
  else if (equalsIgnoreCase(command, "mkdir")) commandMkdir(output, arg, false);
  else if (equalsIgnoreCase(command, "rmdir")) commandMkdir(output, arg, true);
  else if (equalsIgnoreCase(command, "cat")) commandCat(output, arg);
  else if (equalsIgnoreCase(command, "put")) commandPut(input, output, arg, service_network);
  else if (equalsIgnoreCase(command, "rm")) commandRemove(output, arg);
  else if (equalsIgnoreCase(command, "echo")) {
    if (!arg) output.println(("Hasznalat: echo on|off"));
    else if (equalsIgnoreCase(arg, "on") || equalsIgnoreCase(arg, "off")) {
      taskENTER_CRITICAL(); state.echo_enabled = equalsIgnoreCase(arg, "on"); taskEXIT_CRITICAL();
      output.println(("OK"));
    } else output.println(("Hasznalat: echo on|off"));
  } else output.println(("Ismeretlen parancs. help = parancslista"));
  if (shellMutex) xSemaphoreGive(shellMutex);
}

void consoleTask(void*) {
  char line[kLineCapacity]{};
  size_t used = 0;
  bool overflow = false;
  Serial.println(("Teensy 4.x FreeRTOS mikroszamitogep – kezdeti shell"));
  Serial.println(("USB CDC konzol; help = parancsok"));
  printPrompt(Serial);
  for (;;) {
    while (Serial.available() > 0) {
      const char c = static_cast<char>(Serial.read());
      const RuntimeState s = snapshot();
      if (c == '\r' || c == '\n') {
        if (used || overflow) {
          line[used] = '\0';
          if (s.echo_enabled) Serial.println();
          if (overflow) Serial.println(("Hiba: tul hosszu parancssor (max 127 karakter)"));
          else processCommand(Serial, Serial, line, false);
          used = 0;
          overflow = false;
          printPrompt(Serial);
        }
      } else if (c == '\b' || c == 127) {
        if (used) { --used; if (s.echo_enabled) Serial.print(("\b \b")); }
      } else if (c >= 32 && c <= 126) {
        if (used + 1 < sizeof(line)) {
          line[used++] = c;
          if (s.echo_enabled) Serial.write(c);
        } else overflow = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


#if defined(ARDUINO_TEENSY41)
void setNetworkState(bool initialized, bool link, bool listening, const IPAddress& ip) {
  taskENTER_CRITICAL();
  state.ethernet_initialized = initialized;
  state.ethernet_link = link;
  state.tcp_listening = listening;
  state.ethernet_has_ip = (ip[0] | ip[1] | ip[2] | ip[3]) != 0;
  for (uint8_t i = 0; i < 4; ++i) state.ethernet_ip[i] = ip[i];
  taskEXIT_CRITICAL();
}

void handleTcpLine(qn::EthernetClient& client, char* line) {
  while (*line == ' ' || *line == '\t') ++line;
  char* end = line + strlen(line);
  while (end > line && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
  if (!*line) { printPrompt(client); return; }
  if (equalsIgnoreCase(line, "quit")) {
    client.println("bye");
    client.stop();
    return;
  }
  processCommand(client, client, line, true);
  printPrompt(client);
}

void networkTask(void*) {
  qn::Ethernet.setHostname("teensy-os");
  const bool started = qn::Ethernet.begin();  // DHCP; do not block waiting for a lease.
  qn::EthernetServer server{2323};
  const bool listening = started;
  if (listening) server.begin();
  qn::EthernetClient client;
  char line[kLineCapacity]{};
  size_t used = 0;
  bool overflow = false;

  for (;;) {
    qn::Ethernet.loop();  // QNEthernet is configured not to call this from EventResponder/yield.
    const IPAddress ip = qn::Ethernet.localIP();
    setNetworkState(started, qn::Ethernet.linkState(), listening, ip);

    if (!client || !client.connected()) {
      if (client) client.stop();
      used = 0;
      overflow = false;
      client = server.accept();
      if (client) {
        client.println("Teensy 4.x FreeRTOS konzol");
        client.println("Ugyanaz a shell, mint az USB soros porton. help = parancsok.");
        printPrompt(client);
      }
    }

    while (client && client.connected() && client.available() > 0) {
      const char c = static_cast<char>(client.read());
      if (c == '\r' || c == '\n') {
        if (used || overflow) {
          line[used] = '\0';
          if (snapshot().echo_enabled) client.println();
          if (overflow) client.println("Hiba: tul hosszu parancssor (max 127 karakter)");
          else handleTcpLine(client, line);
          used = 0;
          overflow = false;
        }
      } else if (c == 8 || c == 127) {
        if (used) {
          --used;
          if (snapshot().echo_enabled) client.print("\b \b");
        }
      } else if (c >= 32 && c <= 126) {
        if (used + 1 < sizeof(line)) {
          line[used++] = c;
          if (snapshot().echo_enabled) client.write(c);
        }
        else overflow = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
#endif

void clockTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    taskENTER_CRITICAL();
    ++state.uptime_seconds;
    taskEXIT_CRITICAL();
  }
}

void startupTask(void*) {
  bool sd_ready = false;
  while (!sd_ready) {
#if defined(ARDUINO_TEENSY41)
    Serial.println("SD-kartya csatolasa indulaskor...");
    sd_ready = SD.begin(BUILTIN_SDCARD);
#elif defined(ARDUINO_TEENSY40)
    Serial.println("SPI SD-kartya csatolasa indulaskor, CS=10...");
    sd_ready = SD.begin(kDefaultSdChipSelect);
#endif
    taskENTER_CRITICAL();
    state.sd_mounted = sd_ready;
    taskEXIT_CRITICAL();
    if (!sd_ready) {
      Serial.println("HIBA: SD nem olvashato; ujraproba 2 masodperc mulva.");
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }

  Serial.println("SD-kartya csatolva; rendszerinditas folytatodik.");
  shellMutex = xSemaphoreCreateMutex();
  if (!shellMutex) {
    Serial.println("HIBA: shell mutex letrehozasa sikertelen");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  xTaskCreate(consoleTask, "console", kConsoleStackWords, nullptr, kConsolePriority, nullptr);
  xTaskCreate(clockTask, "clock", kClockStackWords, nullptr, kClockPriority, nullptr);
#if defined(ARDUINO_TEENSY41)
  xTaskCreate(networkTask, "network", 1536, nullptr, 2, nullptr);
#endif
  vTaskDelete(nullptr);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  if (xTaskCreate(startupTask, "startup", kStartupStackWords, nullptr, 3, nullptr) != pdPASS) {
    Serial.println("HIBA: indito feladat letrehozasa sikertelen");
    for (;;) delay(1000);
  }
  vTaskStartScheduler();
}

void loop() {}
