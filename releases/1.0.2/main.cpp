#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============ CONFIGURATION ============
#define FIRMWARE_VERSION "1.0.2"
#define LED_PIN 2
#define BLINK_DELAY_MS 2000  // 2 seconds for v1.0.2 (previously 100ms in v1.0.1)

// WiFi Credentials
const char* ssid = "TRINITY";
const char* password = "Trinity@unifi";

// Google Drive OTA links (replace FILEID_... with actual file IDs)
const char* otaVersionUrl = "https://drive.google.com/uc?export=download&id=FILEID_VERSION";
const char* otaBinUrl     = "https://drive.google.com/uc?export=download&id=FILEID_BIN";

// NTP Server
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;              // UTC offset in seconds
const int daylightOffset_sec = 0;          // Daylight savings offset

// ============ GLOBAL STATE ============
unsigned long uptime_seconds = 0;
bool wifi_connected = false;
bool ntp_synced = false;
char current_time[32] = "Not synced";

// ============ TASK HANDLES ============
TaskHandle_t wifiTaskHandle = NULL;
TaskHandle_t otaTaskHandle = NULL;
TaskHandle_t blinkTaskHandle = NULL;
TaskHandle_t ntpTaskHandle = NULL;
TaskHandle_t uptimeTaskHandle = NULL;
TaskHandle_t diagnosticsTaskHandle = NULL;

// ============ TASK: WIFI CONNECTION ============
void wifi_connect_task(void *parameter) {
  int retry_count = 0;
  const int max_retries = 10;

  while (1) {
    if (!wifi_connected) {
      retry_count = 0;
      Serial.println("[WiFi Task] Attempting to connect to WiFi TRINITY...");
      
      WiFi.begin(ssid, password);
      
      while (WiFi.status() != WL_CONNECTED && retry_count < max_retries) {
        delay(1000);
        retry_count++;
        Serial.printf("[WiFi Task] Connecting... attempt %d/%d\n", retry_count, max_retries);
      }
      
      if (WiFi.status() == WL_CONNECTED) {
        wifi_connected = true;
        Serial.println("[WiFi Task] WiFi connected successfully!");
        Serial.printf("[WiFi Task] IP: %s\n", WiFi.localIP().toString().c_str());
      } else {
        wifi_connected = false;
        Serial.println("[WiFi Task] WiFi connection failed, retrying in 30s...");
        vTaskDelay(30000 / portTICK_PERIOD_MS);
      }
    } else {
      // Check if still connected
      if (WiFi.status() != WL_CONNECTED) {
        wifi_connected = false;
        Serial.println("[WiFi Task] WiFi disconnected, will reconnect...");
      }
    }
    
    vTaskDelay(10000 / portTICK_PERIOD_MS); // Check every 10 seconds
  }
}

// ============ TASK: NTP TIME SYNC ============
void ntp_sync_task(void *parameter) {
  while (1) {
    if (wifi_connected && !ntp_synced) {
      Serial.println("[NTP Task] Syncing time with NTP server...");
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      
      int sync_attempts = 0;
      time_t now = time(nullptr);
      
      while (now < 24 * 3600 && sync_attempts < 20) {
        delay(500);
        now = time(nullptr);
        sync_attempts++;
      }
      
      if (now > 24 * 3600) {
        ntp_synced = true;
        Serial.println("[NTP Task] Time synchronized successfully!");
      } else {
        Serial.println("[NTP Task] NTP sync failed, retrying...");
        ntp_synced = false;
      }
    }
    
    vTaskDelay(30000 / portTICK_PERIOD_MS); // Check every 30 seconds
  }
}

// ============ TASK: OTA CHECK ============
void ota_check_task(void *parameter) {
  HTTPClient http;
  String remoteVersion;

  while (1) {
    if (wifi_connected) {
      Serial.println("[OTA Task] Checking for firmware updates...");
      Serial.printf("[OTA Task] Current version: %s\n", FIRMWARE_VERSION);

      // fetch version file from Google Drive
      http.begin(otaVersionUrl);
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        remoteVersion = http.getString();
        remoteVersion.trim();
        Serial.printf("[OTA Task] Remote version: %s\n", remoteVersion.c_str());
        if (remoteVersion != String(FIRMWARE_VERSION)) {
          Serial.println("[OTA Task] New firmware detected, starting download...");
          // download bin
          http.end();
          http.begin(otaBinUrl);
          int code = http.GET();
          if (code == HTTP_CODE_OK) {
            int len = http.getSize();
            WiFiClient* stream = http.getStreamPtr();
            if (Update.begin(len)) {
              size_t written = Update.writeStream(*stream);
              if (written == len) {
                Serial.println("[OTA Task] Firmware written, rebooting...");
                Update.end(true);
                ESP.restart();
              } else {
                Serial.printf("[OTA Task] Written only %u/%u bytes\n", written, len);
              }
            } else {
              Serial.println("[OTA Task] Not enough space to begin OTA");
            }
          } else {
            Serial.printf("[OTA Task] Firmware download failed, http code %d\n", code);
          }
        } else {
          Serial.println("[OTA Task] Already at latest version.");
        }
      } else {
        Serial.printf("[OTA Task] Version check failed, http code %d\n", httpCode);
      }
      http.end();
    }
    
    // Check every 60 seconds (60000 ms)
    vTaskDelay(60000 / portTICK_PERIOD_MS);
  }
}

// ============ TASK: LED BLINK ============
void blink_task(void *parameter) {
  pinMode(LED_PIN, OUTPUT);
  
  while (1) {
    digitalWrite(LED_PIN, HIGH);
    vTaskDelay((BLINK_DELAY_MS / 2) / portTICK_PERIOD_MS);
    
    digitalWrite(LED_PIN, LOW);
    vTaskDelay((BLINK_DELAY_MS / 2) / portTICK_PERIOD_MS);
  }
}

// ============ TASK: UPTIME COUNTER ============
void uptime_task(void *parameter) {
  while (1) {
    uptime_seconds++;
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Increment every 1 second
  }
}

// ============ TASK: DIAGNOSTICS & TIME DISPLAY ============
void diagnostics_task(void *parameter) {
  while (1) {
    // Update current time
    if (ntp_synced) {
      time_t now = time(nullptr);
      struct tm *timeinfo = localtime(&now);
      strftime(current_time, sizeof(current_time), "%Y-%m-%d %H:%M:%S", timeinfo);
    }
    
    // Print diagnostics every 1 second
    Serial.println("========================================");
    Serial.printf("[DIAGNOSTICS] Uptime: %lu seconds\n", uptime_seconds);
    Serial.printf("[DIAGNOSTICS] Firmware: %s\n", FIRMWARE_VERSION);
    Serial.printf("[DIAGNOSTICS] Blink Interval: %d ms\n", BLINK_DELAY_MS);
    Serial.printf("[DIAGNOSTICS] WiFi Status: %s\n", wifi_connected ? "CONNECTED" : "DISCONNECTED");
    Serial.printf("[DIAGNOSTICS] NTP Status: %s\n", ntp_synced ? "SYNCED" : "NOT SYNCED");
    Serial.printf("[DIAGNOSTICS] Current Time: %s\n", current_time);
    Serial.printf("[DIAGNOSTICS] Free Heap: %u bytes\n", esp_get_free_heap_size());
    Serial.println("========================================");
    
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Print every 1 second
  }
}

// ============ SETUP ============
void setup() {
  // Initialize Serial
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n========================================");
  Serial.println("ESP32 RTOS Firmware - OTA Project");
  Serial.printf("Version: %s\n", FIRMWARE_VERSION);
  Serial.println("========================================\n");
  
  // Disable WiFi sleep to improve connection stability
  WiFi.setSleep(false);
  
  // Create FreeRTOS Tasks
  xTaskCreate(wifi_connect_task, "WiFi", 4096, NULL, 2, &wifiTaskHandle);
  xTaskCreate(ntp_sync_task, "NTP", 4096, NULL, 2, &ntpTaskHandle);
  xTaskCreate(ota_check_task, "OTA", 4096, NULL, 1, &otaTaskHandle);
  xTaskCreate(blink_task, "Blink", 2048, NULL, 1, &blinkTaskHandle);
  xTaskCreate(uptime_task, "Uptime", 2048, NULL, 3, &uptimeTaskHandle);
  xTaskCreate(diagnostics_task, "Diagnostics", 4096, NULL, 1, &diagnosticsTaskHandle);
  
  Serial.println("[Setup] All tasks created. FreeRTOS scheduler starting...\n");
}

// ============ MAIN LOOP ============
void loop() {
  // When using FreeRTOS, the loop() function is a low-priority task
  // It can be left empty as all work is done in the created tasks
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}
