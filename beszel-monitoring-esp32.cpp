#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>

// ==================== CONFIG ====================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* BESZEL_URL    = "https://your.beszel.domain";
const char* BESZEL_EMAIL  = "your@email.com";
const char* BESZEL_PASS   = "yourpassword";
const char* SYSTEM_ID     = "np97pp5jn9tfe99";
// ================================================

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Auth
String authToken = "";
unsigned long tokenExpiry = 0;

// Stats
float vmCpu = 0, vmRam = 0, vmDisk = 0;
float netKB = 0;
uint32_t vmUptime = 0;

struct Container {
  String name;
  float cpu;
  float mem;
};
Container containers[10];
int containerCount = 0;

// Display
int currentScreen = 0;
unsigned long lastScreenSwitch = 0;
unsigned long lastDataFetch = 0;
const int SCREEN_INTERVAL = 5000;  // 5 seconds per screen
const int FETCH_INTERVAL  = 30000; // fetch every 30 seconds

// ─── Auth ────────────────────────────────────────
bool authenticate() {
  HTTPClient http;
  String url = String(BESZEL_URL) + "/api/collections/users/auth-with-password";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  String body = "{\"identity\":\"" + String(BESZEL_EMAIL) +
                "\",\"password\":\"" + String(BESZEL_PASS) + "\"}";

  int code = http.POST(body);
  if (code != 200) {
    Serial.println("Auth failed: " + String(code));
    http.end();
    return false;
  }

  DynamicJsonDocument doc(2048);
  deserializeJson(doc, http.getString());
  authToken = doc["token"].as<String>();
  tokenExpiry = millis() + 3600000; // 1 hour
  http.end();
  Serial.println("Auth OK");
  return true;
}

// ─── Fetch VM Stats ──────────────────────────────
void fetchVMStats() {
  if (millis() > tokenExpiry) authenticate();

  HTTPClient http;
  String url = String(BESZEL_URL) + "/api/collections/systems/records/" + SYSTEM_ID;
  http.begin(url);
  http.addHeader("Authorization", authToken);

  int code = http.GET();
  if (code != 200) {
    Serial.println("VM fetch failed: " + String(code));
    http.end();
    return;
  }

  DynamicJsonDocument doc(4096);
  deserializeJson(doc, http.getString());

  JsonObject info = doc["info"];
  vmCpu    = info["cpu"]  | 0.0f;
  vmRam    = info["mp"]   | 0.0f;
  vmDisk   = info["dp"]   | 0.0f;
  vmUptime = info["u"]    | 0;
  netKB    = info["bb"]   | 0.0f;
  netKB    = netKB / 1024.0f; // bytes to KB

  http.end();
  Serial.printf("VM: CPU=%.1f%% RAM=%.1f%% Disk=%.1f%%\n", vmCpu, vmRam, vmDisk);
}

// ─── Fetch Containers ────────────────────────────
void fetchContainers() {
  HTTPClient http;
  String url = String(BESZEL_URL) +
               "/api/collections/containers/records?filter=(system='" +
               SYSTEM_ID + "')&perPage=20";
  http.begin(url);
  http.addHeader("Authorization", authToken);

  int code = http.GET();
  if (code != 200) {
    Serial.println("Container fetch failed: " + String(code));
    http.end();
    return;
  }

  DynamicJsonDocument doc(8192);
  deserializeJson(doc, http.getString());

  JsonArray items = doc["items"].as<JsonArray>();
  containerCount = 0;
  for (JsonObject item : items) {
    if (containerCount >= 10) break;
    containers[containerCount].name = item["name"].as<String>();
    containers[containerCount].cpu  = item["cpu"] | 0.0f;
    containers[containerCount].mem  = item["mem"] | 0.0f;
    containerCount++;
  }

  http.end();
  Serial.printf("Got %d containers\n", containerCount);
}

// ─── Draw Helpers ────────────────────────────────
void drawProgressBar(int x, int y, int w, int h, float pct) {
  u8g2.drawFrame(x, y, w, h);
  int fill = constrain((int)(pct / 100.0f * (w - 2)), 0, w - 2);
  if (fill > 0) u8g2.drawBox(x + 1, y + 1, fill, h - 2);
}

String formatUptime(uint32_t secs) {
  uint32_t d = secs / 86400;
  uint32_t h = (secs % 86400) / 3600;
  char buf[16];
  sprintf(buf, "%lud %luh", d, h);
  return String(buf);
}

// ─── Screen 1: VM Stats ──────────────────────────
void drawVMScreen() {
  char buf[32];
  u8g2.clearBuffer();

  // Title
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(10, 10, "localhost:oracle");
  u8g2.drawLine(0, 12, 128, 12);

  u8g2.setFont(u8g2_font_5x7_tr);

  // CPU
  u8g2.drawStr(0, 23, "CPU");
  drawProgressBar(22, 16, 80, 8, vmCpu);
  sprintf(buf, "%.1f%%", vmCpu);
  u8g2.drawStr(105, 23, buf);

  // RAM
  u8g2.drawStr(0, 35, "RAM");
  drawProgressBar(22, 28, 80, 8, vmRam);
  sprintf(buf, "%.1f%%", vmRam);
  u8g2.drawStr(105, 35, buf);

  // Disk
  u8g2.drawStr(0, 47, "DSK");
  drawProgressBar(22, 40, 80, 8, vmDisk);
  sprintf(buf, "%.1f%%", vmDisk);
  u8g2.drawStr(105, 47, buf);

  // Uptime + Network
  u8g2.drawStr(0, 60, ("Up:" + formatUptime(vmUptime)).c_str());
  sprintf(buf, "%.1fKB/s", netKB);
  u8g2.drawStr(75, 60, buf);

  u8g2.sendBuffer();
}

// ─── Screen 2 & 3: Containers ────────────────────
void drawContainerScreen(int page) {
  // 4 containers per page
  int start = page * 4;
  int end   = min(start + 4, containerCount);
  int total = (containerCount + 3) / 4;

  char buf[32];
  u8g2.clearBuffer();

  // Title
  u8g2.setFont(u8g2_font_6x10_tr);
  sprintf(buf, "Docker (%d/%d)", page + 1, total);
  u8g2.drawStr(20, 10, buf);
  u8g2.drawLine(0, 12, 128, 12);

  u8g2.setFont(u8g2_font_5x7_tr);

  for (int i = start; i < end; i++) {
    int y = 22 + (i - start) * 13;
    String name = containers[i].name;
    if (name.length() > 13) name = name.substring(0, 13);
    u8g2.drawStr(0, y, name.c_str());
    sprintf(buf, "%.2f%%", containers[i].cpu);
    u8g2.drawStr(95, y, buf);
  }

  u8g2.sendBuffer();
}

// ─── Loading Screen ──────────────────────────────
void drawLoading(const char* msg) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(20, 30, "Beszel Monitor");
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(10, 48, msg);
  u8g2.sendBuffer();
}

// ─── Setup ───────────────────────────────────────
void setup() {
  Serial.begin(115200);
  u8g2.begin();

  drawLoading("Connecting WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

  drawLoading("Authenticating...");
  authenticate();

  drawLoading("Fetching stats...");
  fetchVMStats();
  fetchContainers();

  lastDataFetch = millis();
  lastScreenSwitch = millis();
}

// ─── Loop ────────────────────────────────────────
void loop() {
  // Refresh data every 30 seconds
  if (millis() - lastDataFetch > FETCH_INTERVAL) {
    fetchVMStats();
    fetchContainers();
    lastDataFetch = millis();
  }

  // Switch screen every 5 seconds
  int totalScreens = 1 + (containerCount + 3) / 4;
  if (millis() - lastScreenSwitch > SCREEN_INTERVAL) {
    currentScreen = (currentScreen + 1) % totalScreens;
    lastScreenSwitch = millis();
  }

  // Draw current screen
  if (currentScreen == 0) {
    drawVMScreen();
  } else {
    drawContainerScreen(currentScreen - 1);
  }

  delay(100);
}