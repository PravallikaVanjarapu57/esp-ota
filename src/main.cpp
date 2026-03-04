#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Update.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============ FIRMWARE VERSION ============
#define FIRMWARE_VERSION "1.0.5"

// ============ CONFIGURATION ============
#ifndef LED_BUILTIN
  #define LED_BUILTIN 2
#endif

#define LED_PIN        LED_BUILTIN
#define BLINK_DELAY_MS 2000

// WiFi Credentials
const char* ssid     = "TRINITY";
const char* password = "Trinity@unifi";

// GitHub OTA URLs
const char* otaVersionUrl =
  "https://github.com/PravallikaVanjarapu57/esp-ota/releases/latest/download/version.txt";
const char* otaBinUrl =
  "https://github.com/PravallikaVanjarapu57/esp-ota/releases/latest/download/firmware.bin";

// NTP
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = 0;
const int   daylightOffset_sec = 0;

// ============ GLOBAL STATE ============
Preferences preferences;
String currentVersion = FIRMWARE_VERSION;

unsigned long uptime_seconds  = 0;
volatile bool wifi_connected  = false;
volatile bool internet_ok     = false;
bool          ntp_synced      = false;
char          current_time[32]= "Not synced";

// ============ TASK HANDLES ============
TaskHandle_t wifiTaskHandle        = NULL;
TaskHandle_t otaTaskHandle         = NULL;
TaskHandle_t blinkTaskHandle       = NULL;
TaskHandle_t ntpTaskHandle         = NULL;
TaskHandle_t uptimeTaskHandle      = NULL;
TaskHandle_t diagnosticsTaskHandle = NULL;
TaskHandle_t internetTaskHandle    = NULL;

// ============ HELPERS ============

bool isNewerVersion(const String& localVer, const String& remoteVer) {
  if (localVer == remoteVer) return false;

  int lMaj = 0, lMin = 0, lPat = 0;
  int rMaj = 0, rMin = 0, rPat = 0;

  sscanf(localVer.c_str(),  "%d.%d.%d", &lMaj, &lMin, &lPat);
  sscanf(remoteVer.c_str(), "%d.%d.%d", &rMaj, &rMin, &rPat);

  if (rMaj != lMaj) return rMaj > lMaj;
  if (rMin != lMin) return rMin > lMin;
  return rPat > lPat;
}

// Fetch URL using insecure HTTPS (skips cert verification — works with GitHub redirects)
String httpsGetString(const char* url, int& httpCode) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) {
    httpCode = -1;
    return "";
  }

  httpCode = http.GET();
  String result = "";
  if (httpCode == HTTP_CODE_OK) {
    result = http.getString();
  }
  http.end();
  return result;
}

// ============ WIFI TASK ============
void wifi_connect_task(void *parameter) {
  int retry_count = 0;
  const int max_retries = 10;

  while (1) {
    if (!wifi_connected) {
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, password);
      retry_count = 0;

      while (WiFi.status() != WL_CONNECTED && retry_count < max_retries) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        retry_count++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        wifi_connected = true;
        Serial.println("[WiFi] Connected: " + WiFi.localIP().toString());
      } else {
        WiFi.disconnect(true);
        Serial.println("[WiFi] Failed, retry in 30s");
        vTaskDelay(30000 / portTICK_PERIOD_MS);
      }
    } else {
      if (WiFi.status() != WL_CONNECTED) {
        wifi_connected = false;
        internet_ok    = false;
        Serial.println("[WiFi] Lost connection, reconnecting...");
        WiFi.disconnect(true);
      }
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ============ INTERNET CHECK TASK ============
void internet_check_task(void *parameter) {
  while (1) {
    if (wifi_connected) {
      WiFiClient plain;
      HTTPClient http;
      http.setTimeout(5000);
      if (http.begin(plain, "http://clients3.google.com/generate_204")) {
        int code    = http.GET();
        internet_ok = (code == 204);
        http.end();
      } else {
        internet_ok = false;
      }
    } else {
      internet_ok = false;
    }
    vTaskDelay(10000 / portTICK_PERIOD_MS);
  }
}

// ============ NTP TASK ============
void ntp_sync_task(void *parameter) {
  while (1) {
    if (wifi_connected && !ntp_synced) {
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

      time_t now   = time(nullptr);
      int attempts = 0;
      while (now < 24 * 3600 && attempts < 20) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        now = time(nullptr);
        attempts++;
      }

      if (now > 24 * 3600) {
        ntp_synced = true;
        Serial.println("[NTP] Time synced");
      } else {
        Serial.println("[NTP] Sync failed, retry in 30s");
      }
    }
    vTaskDelay(30000 / portTICK_PERIOD_MS);
  }
}

// ============ OTA TASK ============
void ota_check_task(void *parameter) {
  vTaskDelay(20000 / portTICK_PERIOD_MS); // Wait for internet before first check

  while (1) {
    if (wifi_connected && internet_ok) {

      Serial.println("[OTA] Checking for update...");
      Serial.println("[OTA] Running version: " + currentVersion);

      // Step 1: Fetch version.txt from GitHub Release
      int httpCode = 0;
      String remoteVersion = httpsGetString(otaVersionUrl, httpCode);

      if (httpCode != HTTP_CODE_OK || remoteVersion.isEmpty()) {
        Serial.printf("[OTA] Version fetch failed (HTTP %d)\n", httpCode);
        vTaskDelay(60000 / portTICK_PERIOD_MS);
        continue;
      }

      remoteVersion.trim();
      Serial.println("[OTA] Remote version : " + remoteVersion);
      Serial.println("[OTA] Current version: " + currentVersion);

      // Step 2: Compare versions
      if (!isNewerVersion(currentVersion, remoteVersion)) {
        Serial.println("[OTA] Already up to date");
        vTaskDelay(60000 / portTICK_PERIOD_MS);
        continue;
      }

      Serial.println("[OTA] New version found! Downloading firmware...");

      // Step 3: Download firmware.bin
      WiFiClientSecure client;
      client.setInsecure();

      HTTPClient httpUpdate;
      httpUpdate.setTimeout(60000);
      httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

      if (!httpUpdate.begin(client, otaBinUrl)) {
        Serial.println("[OTA] Failed to begin download");
        vTaskDelay(60000 / portTICK_PERIOD_MS);
        continue;
      }

      int binCode = httpUpdate.GET();
      if (binCode != HTTP_CODE_OK) {
        Serial.printf("[OTA] Firmware download failed (HTTP %d)\n", binCode);
        httpUpdate.end();
        vTaskDelay(60000 / portTICK_PERIOD_MS);
        continue;
      }

      int contentLength = httpUpdate.getSize();
      Serial.printf("[OTA] Firmware size: %d bytes\n", contentLength);

      if (contentLength <= 0) {
        Serial.println("[OTA] Invalid content length");
        httpUpdate.end();
        vTaskDelay(60000 / portTICK_PERIOD_MS);
        continue;
      }

      WiFiClient* stream = httpUpdate.getStreamPtr();
      if (!stream) {
        Serial.println("[OTA] No stream available");
        httpUpdate.end();
        vTaskDelay(60000 / portTICK_PERIOD_MS);
        continue;
      }

      // Step 4: Flash
      if (!Update.begin(contentLength)) {
        Serial.printf("[OTA] Update.begin failed: %s\n", Update.errorString());
        httpUpdate.end();
        vTaskDelay(60000 / portTICK_PERIOD_MS);
        continue;
      }

      Serial.println("[OTA] Flashing... do not power off!");
      size_t written = Update.writeStream(*stream);
      httpUpdate.end();

      if (written != (size_t)contentLength) {
        Serial.printf("[OTA] Write mismatch: %u of %d bytes. %s\n",
                      written, contentLength, Update.errorString());
        Update.abort();
        vTaskDelay(60000 / portTICK_PERIOD_MS);
        continue;
      }

      if (!Update.end(true)) {
        Serial.printf("[OTA] Finalize failed: %s\n", Update.errorString());
        vTaskDelay(60000 / portTICK_PERIOD_MS);
        continue;
      }

      // Step 5: Save version and reboot
      preferences.putString("version", remoteVersion);
      Serial.printf("[OTA] Update to v%s successful! Rebooting...\n",
                    remoteVersion.c_str());
      delay(1000);
      ESP.restart();

    } else {
      Serial.println("[OTA] Skipping - no internet");
    }

    vTaskDelay(60000 / portTICK_PERIOD_MS);
  }
}

// ============ LED BLINK TASK ============
void blink_task(void *parameter) {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  while (1) {
    if (wifi_connected && internet_ok) {
      digitalWrite(LED_PIN, HIGH);
      vTaskDelay((BLINK_DELAY_MS / 2) / portTICK_PERIOD_MS);
      digitalWrite(LED_PIN, LOW);
      vTaskDelay((BLINK_DELAY_MS / 2) / portTICK_PERIOD_MS);
    } else if (wifi_connected) {
      digitalWrite(LED_PIN, HIGH);
      vTaskDelay(800 / portTICK_PERIOD_MS);
      digitalWrite(LED_PIN, LOW);
      vTaskDelay(200 / portTICK_PERIOD_MS);
    } else {
      digitalWrite(LED_PIN, LOW);
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }
  }
}

// ============ UPTIME TASK ============
void uptime_task(void *parameter) {
  while (1) {
    uptime_seconds++;
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ============ DIAGNOSTICS TASK ============
void diagnostics_task(void *parameter) {
  while (1) {
    if (ntp_synced) {
      time_t now = time(nullptr);
      struct tm* timeinfo = localtime(&now);
      strftime(current_time, sizeof(current_time), "%Y-%m-%d %H:%M:%S", timeinfo);
    }

    Serial.println("=================================");
    Serial.printf("Uptime   : %lu sec\n",  uptime_seconds);
    Serial.printf("Firmware : v%s\n",      currentVersion.c_str());
    Serial.printf("WiFi     : %s\n",       wifi_connected ? "OK" : "NO");
    Serial.printf("Internet : %s\n",       internet_ok    ? "OK" : "NO");
    Serial.printf("Time     : %s\n",       current_time);
    Serial.printf("Free Heap: %u bytes\n", esp_get_free_heap_size());
    Serial.println("=================================");

    vTaskDelay(10000 / portTICK_PERIOD_MS);
  }
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(1000);

  preferences.begin("ota", false);

  String savedVersion = preferences.getString("version", "");
  if (savedVersion.isEmpty()) {
    preferences.putString("version", FIRMWARE_VERSION);
    currentVersion = FIRMWARE_VERSION;
  } else {
    currentVersion = savedVersion;
  }

  Serial.println("\n=============================");
  Serial.println("  ESP32 Dynamic OTA Firmware  ");
  Serial.printf ("  Running version: v%s\n", currentVersion.c_str());
  Serial.println("=============================\n");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  WiFi.setSleep(false);

  xTaskCreate(wifi_connect_task,   "WiFi",     4096,  NULL, 3, &wifiTaskHandle);
  xTaskCreate(internet_check_task, "Internet", 4096,  NULL, 2, &internetTaskHandle);
  xTaskCreate(ntp_sync_task,       "NTP",      4096,  NULL, 2, &ntpTaskHandle);
  xTaskCreate(ota_check_task,      "OTA",      16384, NULL, 1, &otaTaskHandle);
  xTaskCreate(blink_task,          "Blink",    2048,  NULL, 1, &blinkTaskHandle);
  xTaskCreate(uptime_task,         "Uptime",   2048,  NULL, 3, &uptimeTaskHandle);
  xTaskCreate(diagnostics_task,    "Diag",     4096,  NULL, 1, &diagnosticsTaskHandle);
}

void loop() {
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}