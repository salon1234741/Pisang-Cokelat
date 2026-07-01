#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <time.h>

// =========================================================================
// 1. KONFIGURASI WI-FI & API
// =========================================================================
const char* ssid     = "WIFI_SSID";       // Ganti dengan SSID WiFi
const char* password = "WIFI_PASSWORD";  // Ganti dengan password WiFi
const char* witToken = "WIT_AI_TOKEN";   // Ganti dengan token Wit.ai

const char* groqApiKey = "GROQ_API_KEY"; // Ganti dengan API key dari console.groq.com
const char* groqModel  = "llama-3.1-8b-instant";
const char* groqHost   = "api.groq.com";

const char* systemInstruction =
  "Kamu adalah asisten suara ramah berbahasa Indonesia. "
  "Berikan jawaban yang jelas, detail, dan informatif sekitar 1-2 paragraf pendek.";

#define OLED_PAGE_DELAY 2000  // milidetik per halaman OLED

// =========================================================================
// 1b. KONFIGURASI NTP
// =========================================================================
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;   // WIB = UTC+7
const int   daylightOffset_sec = 0;

// =========================================================================
// 1c. KONFIGURASI TOMBOL
// =========================================================================
// GPIO 0 = tombol BOOT bawaan ESP32-S3 (active LOW)
// Ganti pin ini jika pakai tombol eksternal
#define BUTTON_PIN 0
#define BUTTON_ACTIVE LOW  // LOW saat ditekan

WiFiClientSecure secureClient;

// Histori percakapan (4 giliran = hemat RAM & token)
#define MAX_TURNS 4
String historyRole[MAX_TURNS];
String historyText[MAX_TURNS];
int historyCount = 0;

// =========================================================================
// 2. KONFIGURASI HARDWARE
// =========================================================================
#define I2S_WS   5
#define I2S_SCK  6
#define I2S_SD   7
#define I2S_PORT I2S_NUM_0

#define OLED_SDA 8
#define OLED_SCL 9

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

// =========================================================================
// 3. KONFIGURASI AUDIO
// =========================================================================
#define SAMPLE_RATE      16000
#define MAX_RECORD_SEC   10
#define MAX_BUFFER_SIZE  (SAMPLE_RATE * MAX_RECORD_SEC)
#define I2S_READ_CHUNK   512

int16_t *audioBuffer = NULL;
uint32_t samplesRecorded = 0;
int32_t pcm_filter_dc = 0;

// Deklarasi fungsi
void init_i2s();
void init_oled();
void initNTP();
bool isButtonPressed();
void showClock();
uint32_t recordWhileButtonHeld();
String sendToWitAIRaw(uint32_t sampleCount);
String askGroq(String userText);
void addHistory(const char* role, const String& text);
void oledShowStatus(const char* line1, const char* line2 = "");
void oledShowWrappedText(const char* title, const String& text);
void oledShowPaged(const char* title, const String& text);

void setup() {
  Serial.begin(115200);

  // Tombol
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  init_oled();
  oledShowStatus("Booting...");

  audioBuffer = (int16_t *)malloc(MAX_BUFFER_SIZE * sizeof(int16_t));
  if (!audioBuffer) {
    oledShowStatus("ERROR!", "Gagal alokasi RAM");
    while (1);
  }

  oledShowStatus("Menghubungkan", "WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  Serial.println("Wi-Fi Terhubung");
  oledShowStatus("WiFi Tersambung");

  secureClient.setInsecure();
  init_i2s();
  initNTP();
}

void loop() {
  // Idle: tampilkan jam NTP
  // Cek tombol setiap iterasi
  if (isButtonPressed()) {
    // Debounce
    delay(50);
    if (!isButtonPressed()) return;

    // Tombol ditekan — mulai rekam
    oledShowStatus(">> BICARA <<", "Lepas untuk kirim");
    Serial.println("[REC] Tombol ditekan, merekam...");

    uint32_t recorded = recordWhileButtonHeld();

    if (recorded < SAMPLE_RATE / 2) {
      // Kurang dari 0.5 detik — terlalu pendek
      oledShowStatus("Terlalu pendek", "Tahan lebih lama");
      delay(1500);
      return;
    }

    Serial.printf("[REC] Selesai, %d samples (%.1f detik)\n", recorded, (float)recorded / SAMPLE_RATE);
    oledShowStatus("Memproses...", "Mengirim ke Wit.ai");

    String userText = sendToWitAIRaw(recorded);

    if (userText.length() == 0) {
      oledShowStatus("Tidak dikenali", "Coba lagi...");
      delay(1500);
      return;
    }

    Serial.println("User: " + userText);
    oledShowWrappedText("Kamu:", userText);
    delay(1000);

    oledShowStatus("AI berpikir...");
    String aiText = askGroq(userText);

    if (aiText.length() > 0) {
      Serial.println("AI: " + aiText);
      oledShowPaged("AI:", aiText);
    } else {
      oledShowStatus("Gagal!", "AI tidak merespons");
      delay(3000);
    }
    return;
  }

  // Tidak ada tombol ditekan — tampilkan jam
  showClock();
  delay(500);
}

// =========================================================================
// Tombol
// =========================================================================
bool isButtonPressed() {
  return digitalRead(BUTTON_PIN) == BUTTON_ACTIVE;
}

// =========================================================================
// Rekam audio selama tombol ditekan (push-to-talk)
// =========================================================================
uint32_t recordWhileButtonHeld() {
  samplesRecorded = 0;
  pcm_filter_dc = 0;
  int32_t i2s_buf[I2S_READ_CHUNK];

  while (isButtonPressed() && samplesRecorded < MAX_BUFFER_SIZE) {
    size_t bytes_read = 0;
    i2s_read(I2S_PORT, i2s_buf, sizeof(i2s_buf), &bytes_read, portMAX_DELAY);
    int count = bytes_read / sizeof(int32_t);

    for (int j = 0; j < count && samplesRecorded < MAX_BUFFER_SIZE; j++) {
      int16_t s = (int16_t)(i2s_buf[j] >> 14);
      pcm_filter_dc += (s - pcm_filter_dc) >> 2;
      audioBuffer[samplesRecorded++] = s - (int16_t)pcm_filter_dc;
    }
  }

  return samplesRecorded;
}

// =========================================================================
// NTP & Jam
// =========================================================================
void initNTP() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("NTP dikonfigurasi (WIB UTC+7)");
}

void showClock() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    oledShowStatus("Jam", "Sinkronisasi NTP...");
    return;
  }

  char timeBuf[9];   // "HH:MM:SS"
  char dateBuf[20];  // "Sen, 01 Jul 2026"

  strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &timeinfo);

  // Nama hari Indonesia
  const char* hariID[] = {"Min", "Sen", "Sel", "Rab", "Kam", "Jum", "Sab"};
  const char* bulanID[] = {"Jan", "Feb", "Mar", "Apr", "Mei", "Jun",
                           "Jul", "Agu", "Sep", "Okt", "Nov", "Des"};

  snprintf(dateBuf, sizeof(dateBuf), "%s, %02d %s %04d",
           hariID[timeinfo.tm_wday],
           timeinfo.tm_mday,
           bulanID[timeinfo.tm_mon],
           timeinfo.tm_year + 1900);

  u8g2.clearBuffer();

  // Jam besar di tengah
  u8g2.setFont(u8g2_font_logisoso22_tn);
  int timeW = u8g2.getStrWidth(timeBuf);
  u8g2.drawStr((128 - timeW) / 2, 30, timeBuf);

  // Tanggal di bawah
  u8g2.setFont(u8g2_font_6x12_tf);
  int dateW = u8g2.getStrWidth(dateBuf);
  u8g2.drawStr((128 - dateW) / 2, 48, dateBuf);

  // Hint tombol
  u8g2.setFont(u8g2_font_5x7_tf);
  const char* hint = "Tekan tombol utk bicara";
  int hintW = u8g2.getStrWidth(hint);
  u8g2.drawStr((128 - hintW) / 2, 62, hint);

  u8g2.sendBuffer();
}

// =========================================================================
// I2S
// =========================================================================
void init_i2s() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = false
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  i2s_start(I2S_PORT);
}

// =========================================================================
// OLED
// =========================================================================
void init_oled() {
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x12_tf);
}

void oledShowStatus(const char* line1, const char* line2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x14B_tf);
  u8g2.drawStr(0, 16, line1);
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 34, line2);
  u8g2.sendBuffer();
}

int oledShowWrappedPage(const char* title, const String& text, int startPos, int page, int totalPages) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);

  if (totalPages > 1) {
    u8g2.drawStr(0, 10, title);
    String pageInfo = String(page) + "/" + String(totalPages);
    int pw = pageInfo.length() * 6;
    u8g2.drawStr(128 - pw, 10, pageInfo.c_str());
  } else {
    u8g2.drawStr(0, 10, title);
  }
  u8g2.drawHLine(0, 13, 128);

  const int maxChars = 21;
  const int lineH = 12;
  const int maxLines = 3;
  int y = 26;
  int start = startPos;
  int len = text.length();
  int linesDrawn = 0;

  while (start < len && linesDrawn < maxLines) {
    int end = start + maxChars;
    if (end >= len) {
      end = len;
    } else {
      int sp = text.lastIndexOf(' ', end);
      if (sp > start) end = sp;
    }

    String line = text.substring(start, end);
    line.trim();
    u8g2.drawStr(0, y, line.c_str());

    y += lineH;
    linesDrawn++;
    start = end;
    while (start < len && text.charAt(start) == ' ') start++;
  }

  u8g2.sendBuffer();

  if (start >= len) return -1;
  return start;
}

int countPages(const String& text) {
  const int maxChars = 21;
  const int maxLines = 3;
  int start = 0;
  int len = text.length();
  int pages = 0;

  while (start < len) {
    int linesDrawn = 0;
    while (start < len && linesDrawn < maxLines) {
      int end = start + maxChars;
      if (end >= len) {
        end = len;
      } else {
        int sp = text.lastIndexOf(' ', end);
        if (sp > start) end = sp;
      }
      linesDrawn++;
      start = end;
      while (start < len && text.charAt(start) == ' ') start++;
    }
    pages++;
  }
  return pages > 0 ? pages : 1;
}

void oledShowPaged(const char* title, const String& text) {
  int totalPages = countPages(text);
  int pos = 0;
  int page = 1;

  while (pos >= 0) {
    pos = oledShowWrappedPage(title, text, pos, page, totalPages);
    delay(OLED_PAGE_DELAY);
    page++;
  }
}

void oledShowWrappedText(const char* title, const String& text) {
  oledShowWrappedPage(title, text, 0, 1, 1);
}

// =========================================================================
// Histori percakapan
// =========================================================================
void addHistory(const char* role, const String& text) {
  if (historyCount < MAX_TURNS) {
    historyRole[historyCount] = role;
    historyText[historyCount] = text;
    historyCount++;
  } else {
    for (int i = 1; i < MAX_TURNS; i++) {
      historyRole[i - 1] = historyRole[i];
      historyText[i - 1] = historyText[i];
    }
    historyRole[MAX_TURNS - 1] = role;
    historyText[MAX_TURNS - 1] = text;
  }
}

// =========================================================================
// Wit.ai Speech-to-Text
// =========================================================================
String sendToWitAIRaw(uint32_t sampleCount) {
  String finalText = "";

  if (WiFi.status() != WL_CONNECTED) {
    oledShowStatus("ERROR!", "WiFi terputus");
    return finalText;
  }

  HTTPClient http;
  http.setTimeout(15000);
  http.begin("https://api.wit.ai/speech?v=20260701");
  http.addHeader("Authorization", String("Bearer ") + witToken);
  http.addHeader("Content-Type", "audio/raw;encoding=signed-integer;bits=16;rate=16000;endian=little");

  uint32_t dataLen = sampleCount * sizeof(int16_t);
  int httpCode = http.POST((uint8_t*)audioBuffer, dataLen);

  if (httpCode == 200 || httpCode == 201) {
    String response = http.getString();

    int searchPos = 0;
    while (true) {
      int braceStart = response.indexOf('{', searchPos);
      if (braceStart < 0) break;

      int depth = 0, braceEnd = -1;
      for (int i = braceStart; i < (int)response.length(); i++) {
        char c = response.charAt(i);
        if (c == '{') depth++;
        else if (c == '}' && --depth == 0) { braceEnd = i; break; }
      }
      if (braceEnd < 0) break;

      String chunk = response.substring(braceStart, braceEnd + 1);
      StaticJsonDocument<1024> doc;
      if (!deserializeJson(doc, chunk) && doc.containsKey("text")) {
        finalText = doc["text"].as<String>();
      }
      searchPos = braceEnd + 1;
    }
  } else {
    Serial.printf("Wit.ai error: %d\n", httpCode);
    oledShowStatus("Gagal kirim!", ("HTTP " + String(httpCode)).c_str());
  }

  http.end();
  return finalText;
}

// =========================================================================
// Groq LLM
// =========================================================================
String askGroq(String userText) {
  String aiText = "";

  if (WiFi.status() != WL_CONNECTED) return aiText;

  addHistory("user", userText);

  DynamicJsonDocument reqDoc(4096);
  reqDoc["model"] = groqModel;
  reqDoc["max_tokens"] = 300;
  reqDoc["temperature"] = 0.7;

  JsonArray messages = reqDoc.createNestedArray("messages");

  JsonObject sysMsg = messages.createNestedObject();
  sysMsg["role"] = "system";
  sysMsg["content"] = systemInstruction;

  for (int i = 0; i < historyCount; i++) {
    JsonObject turn = messages.createNestedObject();
    turn["role"] = (historyRole[i] == "model") ? "assistant" : historyRole[i];
    turn["content"] = historyText[i];
  }

  String body;
  serializeJson(reqDoc, body);

  HTTPClient http;
  http.setTimeout(15000);
  http.begin(secureClient, String("https://") + groqHost + "/openai/v1/chat/completions");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + groqApiKey);

  int httpCode = http.POST(body);

  if (httpCode == 200) {
    String response = http.getString();
    DynamicJsonDocument resDoc(4096);
    if (!deserializeJson(resDoc, response)) {
      aiText = resDoc["choices"][0]["message"]["content"].as<String>();
      aiText.trim();
    }
  } else {
    Serial.printf("Groq error: %d\n", httpCode);
  }

  http.end();

  if (aiText.length() > 0) {
    addHistory("model", aiText);
  }

  return aiText;
}
