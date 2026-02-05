/*
 * Smart Filament Holder
 * ESP32-C6 + GC9A01 + LVGL
 * 
 * Debug commands (DEBUG_MODE):
 * 1-6    - switch screen
 * w850   - set weight 850g
 * t      - tare
 * profile / noprofile - toggle profile
 */

#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <MFRC522.h>
#include <HX711.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config.h"

// ============================================
// FORWARD DECLARATIONS (для Arduino preprocessor)
// ============================================
enum UIState {
  STATE_BOOT = 1,
  STATE_WAIT_SPOOL = 2,
  STATE_SELECT_METHOD = 3,
  STATE_WAIT_NFC = 4,
  STATE_WAIT_APP = 5,
  STATE_RUNNING = 6
};

bool isValidTransition(UIState from, UIState to);
bool transitionToState(UIState newState);

// ============================================
// СИНХРОНИЗАЦИЯ И БЕЗОПАСНОСТЬ
// ============================================
SemaphoreHandle_t stateMutex = NULL;      // Mutex для состояния и профиля
SemaphoreHandle_t spiMutex = NULL;        // Mutex для SPI (LCD/NFC)

// Максимальный размер JSON для валидации
#define MAX_JSON_SIZE 2048

#define TOUCH_I2C_ADDR 0x15

struct TouchPoint {
  int16_t x;
  int16_t y;
  bool touched;
} touchPoint;

volatile bool touch_flag = false;

void IRAM_ATTR touch_interrupt() {
  touch_flag = true;
}

void touch_init();
void touch_read();
void my_touchpad_read(lv_indev_t *indev_drv, lv_indev_data_t *data);

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, -1);
Arduino_GC9A01 *gfx = new Arduino_GC9A01(bus, LCD_RST, 0, true);

MFRC522 rfid(RC522_CS, RC522_RST);
HX711 scale;
WebServer server(HTTP_PORT);
BLEServer* pServer = NULL;
BLECharacteristic* pDataChar = NULL;
BLECharacteristic* pCommandChar = NULL;
BLECharacteristic* pDbSyncChar = NULL;

static lv_display_t *disp;
static lv_indev_t *indev;
static uint8_t buf1[LCD_WIDTH * 40 * 2];

lv_obj_t* screen_boot;
lv_obj_t* screen_wait_spool;
lv_obj_t* screen_select_method;
lv_obj_t* screen_wait_nfc;
lv_obj_t* screen_wait_app;
lv_obj_t* screen_running;

lv_obj_t* arc_progress;
lv_obj_t* label_material;
lv_obj_t* label_manufacturer;
lv_obj_t* label_percent_main;
lv_obj_t* label_percent_decimal;
lv_obj_t* label_percent_sign;
lv_obj_t* label_weight_title;
lv_obj_t* label_weight_value;
lv_obj_t* label_length_title;
lv_obj_t* label_length_value;
lv_obj_t* label_time;

struct FilamentData {
  String id = "";
  String material = "";
  String manufacturer = "";
  float weight = 0.0f;
  float spool_weight = 0.0f;
  int length = 0;
  float diameter = 1.75f;
  float density = 1.24f;
  int bed_temp = 60;
} currentFilament;

// Volatile переменные для межпоточного доступа
volatile float currentWeight = 0.0f;
volatile float netWeight = 0.0f;
volatile float current_percent = 75.5f;
volatile int current_length = 250;
volatile int current_screen = 1;
volatile bool profile_loaded = false;
volatile bool deviceConnected = false;
volatile bool sendingProfilesNow = false;  // Защита от конкурентных отправок профилей

// Не-volatile (защищены mutex или используются только в одном потоке)
String current_material = "PLA";
String current_manufacturer = "LIDER-3D";
String lastNfcUid = "";

// UIState уже объявлен выше (forward declaration)
volatile UIState currentState = STATE_BOOT;

// ============================================
// STATE MACHINE - Валидация переходов
// ============================================
bool isValidTransition(UIState from, UIState to) {
  // Разрешаем оставаться в том же состоянии (например, смена профиля в RUNNING)
  if (from == to) return true;

  switch (from) {
    case STATE_BOOT:
      return (to == STATE_WAIT_SPOOL || to == STATE_SELECT_METHOD);
    case STATE_WAIT_SPOOL:
      return (to == STATE_SELECT_METHOD);
    case STATE_SELECT_METHOD:
      return (to == STATE_WAIT_SPOOL || to == STATE_WAIT_NFC ||
              to == STATE_WAIT_APP || to == STATE_RUNNING);
    case STATE_WAIT_NFC:
      return (to == STATE_WAIT_SPOOL || to == STATE_SELECT_METHOD || to == STATE_RUNNING);
    case STATE_WAIT_APP:
      return (to == STATE_WAIT_SPOOL || to == STATE_SELECT_METHOD || to == STATE_RUNNING);
    case STATE_RUNNING:
      return (to == STATE_WAIT_SPOOL || to == STATE_SELECT_METHOD);
    default:
      return false;
  }
}

// Безопасный переход состояния с mutex
bool transitionToState(UIState newState) {
  if (stateMutex == NULL) {
    // Mutex ещё не создан (ранняя инициализация)
    currentState = newState;
    return true;
  }
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    UIState oldState = currentState;
    if (isValidTransition(oldState, newState)) {
      currentState = newState;
      xSemaphoreGive(stateMutex);
      Serial.printf("[STATE] %d -> %d\n", oldState, newState);
      return true;
    } else {
      xSemaphoreGive(stateMutex);
      Serial.printf("[STATE] INVALID: %d -> %d\n", oldState, newState);
      return false;
    }
  }
  return false;
}

void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
void create_all_screens();
void load_screen(int screen_num);
void update_weight(float weight);
void update_percent(float percent);
void update_length(int length);
void update_time();
void update_material_and_manufacturer();
void btn_nfc_event(lv_event_t* e);
void btn_app_event(lv_event_t* e);
void btn_back_event(lv_event_t* e);

void updateWeightSensor();
void checkNFC();
void handleData();
void handleStatus();
void handleTare();
void sendBLEData();
void sendProfileList();
void sendFullProfile(String filamentId);
bool addProfileToDatabase(String profileJson);
String readNfcData();
bool writeNfcData(String filamentId);
float calculateLength(float weight_g, float diameter_mm, float density);
void loadFilamentProfile(String filamentId);
void saveLastFilament();
void loadLastFilament();
void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\nSmart Filament Holder");
  Serial.println("ESP32-C6 + GC9A01 + LVGL\n");

  // Инициализация FreeRTOS примитивов синхронизации
  Serial.print("[0/8] FreeRTOS sync... ");
  stateMutex = xSemaphoreCreateMutex();
  spiMutex = xSemaphoreCreateMutex();
  if (stateMutex && spiMutex) {
    Serial.println("OK");
  } else {
    Serial.println("FAILED!");
  }

  Serial.print("[1/8] LittleFS... ");
  if (!LittleFS.begin(true)) {
    Serial.println("ERROR: LittleFS mount failed");
  } else {
    Serial.println("OK");
    Serial.println("[LittleFS] Files:");
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    int count = 0;
    while(f) {
      Serial.printf("  - %s (%u bytes)\n", f.name(), f.size());
      f = root.openNextFile();
      count++;
    }
    if (count == 0) {
      Serial.println("  (empty)");
    }
    Serial.printf("[LittleFS] Free heap: %d bytes\n", ESP.getFreeHeap());
  }
  
  // Дисплей
  Serial.print("[2/8] Дисплей... ");
  gfx->begin();
  gfx->fillScreen(0x0000);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  Serial.println("✓");
  
  // LVGL
  Serial.print("[3/8] LVGL... ");
  lv_init();
  
  // LVGL v9 API - создание дисплея
  disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
  lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, my_disp_flush);

  Serial.println("✓");

  // Тачскрин - СНАЧАЛА ИНИЦИАЛИЗАЦИЯ I2C
  Serial.print("[3.5/8] Тачскрин (CST816S)... ");
  
  // Сначала сканируем I2C
  Serial.println();
  Serial.println("[I2C] Сканирование шины...");
  Wire.begin(TP_SDA, TP_SCL);
  Wire.setClock(400000);
  
  byte count = 0;
  for (byte i = 1; i < 127; i++) {
    Wire.beginTransmission(i);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C] Найдено устройство: 0x%02X\n", i);
      count++;
    }
  }
  Serial.printf("[I2C] Всего найдено: %d устройств\n", count);
  
  touch_init();
  Serial.println("✓");

  // Touch input - ПОТОМ РЕГИСТРАЦИЯ В LVGL
  Serial.print("[3.6/8] Регистрация LVGL input... ");
  
  // LVGL v9 API - создание input device
  indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);
  
  if (indev == NULL) {
    Serial.println("✗ ОШИБКА регистрации!");
  } else {
    Serial.printf("✓ (indev=%p)\n", (void*)indev);
  }
  
  // UI
  Serial.print("[4/8] Создание экранов... ");
  create_all_screens();
  load_screen(1);  // BOOT
  Serial.println("✓");
  
  // RC522 (NFC)
  Serial.print("[5/8] NFC (RC522)... ");
  SPI.end();
  SPI.begin(LCD_SCK, RC522_MISO, LCD_MOSI, RC522_CS);
  rfid.PCD_Init();
  
  byte version = rfid.PCD_ReadRegister(rfid.VersionReg);
  if (version != 0x00 && version != 0xFF) {
    Serial.printf("✓ (v0x%02X)\n", version);
  } else {
    Serial.println("✗");
  }
  
  // Возврат SPI для дисплея
  SPI.end();
  SPI.begin(LCD_SCK, -1, LCD_MOSI, LCD_CS);
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);
  SPI.setFrequency(40000000);
  
  // HX711
  #if !DEBUG_MODE
  Serial.print("[6/8] HX711 (Тензодатчик)... ");
  scale.begin(HX711_DOUT, HX711_SCK);
  
  // Проверяем готовность БЕЗ блокировки
  delay(100);
  if (scale.is_ready()) {
    scale.set_scale(CALIBRATION_FACTOR);
    scale.tare(10);
    Serial.println("✓");
  } else {
    Serial.println("✗ Не подключен (продолжаем без него)");
  }
  #else
  Serial.println("[6/8] HX711 (DEBUG MODE)");
  Serial.println("     Команды: w<число> или t");
  #endif
  
  // WiFi
  Serial.print("[7/8] WiFi... ");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  Serial.printf("\n[WiFi] Подключение к %s", WIFI_SSID);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < WIFI_CONNECT_TIMEOUT) {
    delay(250);
    Serial.print(".");
    attempts++;
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("✓ IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi] MAC: %s\n", WiFi.macAddress().c_str());
    Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());

    // Настройка NTP для московского времени (UTC+3)
    configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println("[NTP] Синхронизация времени (МСК, UTC+3)");

    // HTTP Server
    server.on("/data", handleData);
    server.on("/status", handleStatus);
    server.on("/tare", HTTP_POST, handleTare);
    server.begin();
    Serial.printf("[HTTP] Сервер запущен на порту %d\n", HTTP_PORT);
  } else {
    Serial.println("✗ Не удалось подключиться");
    Serial.printf("[WiFi] Статус: %d\n", WiFi.status());
  }
  
  // BLE
  Serial.print("[8/8] BLE... ");
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(512);
  pServer = BLEDevice::createServer();
  
  // BLE Server Callbacks
  class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
      Serial.println("\n[BLE] Подключено");
    }
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
      sendingProfilesNow = false;  // Сброс флага при отключении
      Serial.println("\n[BLE] Отключено");
      pServer->startAdvertising();
    }
  };
  
  // Создаём callback один раз (статический)
  static MyServerCallbacks serverCallbacks;
  pServer->setCallbacks(&serverCallbacks);
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  pDataChar = pService->createCharacteristic(
    DATA_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pDataChar->addDescriptor(new BLE2902()); // Добавляем дескриптор для notify
  
  pCommandChar = pService->createCharacteristic(
    COMMAND_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  
  pDbSyncChar = pService->createCharacteristic(
    DB_SYNC_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pDbSyncChar->addDescriptor(new BLE2902()); // Добавляем дескриптор для notify
  
  class MyCharCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      String value = pCharacteristic->getValue().c_str();

      // Валидация размера JSON
      if (value.length() > MAX_JSON_SIZE) {
        Serial.printf("[BLE] ОШИБКА: Слишком большой JSON (%d > %d)\n", value.length(), MAX_JSON_SIZE);
        return;
      }

      Serial.printf("[BLE] Получено (%d байт): %.100s%s\n",
                    value.length(), value.c_str(),
                    value.length() > 100 ? "..." : "");
      
      // Команда TARE
      if (value.indexOf("\"cmd\":\"tare\"") >= 0 || value.indexOf("tare") >= 0) {
        #if !DEBUG_MODE
        scale.tare(10);
        #else
        currentWeight = 0;
        #endif
        Serial.println("[BLE] Tare выполнен");
      }
      // Команда LOAD_FILAMENT
      else if (value.indexOf("\"cmd\":\"load_filament\"") >= 0 || value.indexOf("filament_id") >= 0) {
        int idx = value.indexOf("filament_id\":\"");
        if (idx >= 0) {
          idx += 14;
          int endIdx = value.indexOf("\"", idx);
          if (endIdx > idx) {
            String id = value.substring(idx, endIdx);
            loadFilamentProfile(id);
          }
        }
      }
      // Команда WRITE_NFC
      else if (value.indexOf("\"cmd\":\"write_nfc\"") >= 0) {
        int idx = value.indexOf("\"id\":\"");
        if (idx >= 0) {
          idx += 6;
          int endIdx = value.indexOf("\"", idx);
          if (endIdx > idx) {
            String id = value.substring(idx, endIdx);
            bool success = writeNfcData(id);
            Serial.printf("[NFC] Запись %s\n", success ? "успешна" : "неудачна");
            
            // Отправляем ответ в приложение через DB_SYNC характеристику
            String response = "{\"cmd\":\"write_nfc_response\",\"success\":";
            response += success ? "true" : "false";
            response += ",\"id\":\"";
            response += id;
            response += "\"}";
            pDbSyncChar->setValue(response.c_str());
            pDbSyncChar->notify();
            Serial.printf("[BLE] Отправлен ответ: %s\n", response.c_str());
          }
        }
      }
      // Команда SET_PROFILE (полный профиль в JSON)
      else if (value.indexOf("\"cmd\":\"set_profile\"") >= 0) {
        Serial.println("[BLE] Set profile команда получена");
        
        // ИСПРАВЛЕНИЕ: Используем ArduinoJson вместо indexOf()
        int dataIdx = value.indexOf("\"data\":{");
        if (dataIdx >= 0) {
          int startIdx = value.indexOf("{", dataIdx + 7);
          int endIdx = value.lastIndexOf("}");
          
          if (startIdx >= 0 && endIdx > startIdx) {
            String profileData = value.substring(startIdx, endIdx + 1);
            Serial.printf("[BLE] Profile data: %s\n", profileData.c_str());
            
            // Парсим через ArduinoJson
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, profileData);
            
            if (error) {
              Serial.print("[JSON] Ошибка парсинга BLE профиля: ");
              Serial.println(error.c_str());
              return;
            }
            
            // Защита mutex при записи в разделяемые данные
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              currentFilament.id = doc["id"] | "";
              currentFilament.material = doc["material"] | "?";
              currentFilament.manufacturer = doc["manufacturer"] | "?";
              currentFilament.weight = doc["weight"] | 0.0f;
              currentFilament.spool_weight = doc["spool_weight"] | doc["spoolWeight"] | 0.0f;
              currentFilament.diameter = doc["diameter"] | 1.75f;
              currentFilament.density = doc["density"] | 1.24f;
              currentFilament.bed_temp = doc["bed_temp"] | doc["bedTemp"] | 60;

              current_material = currentFilament.material;
              current_manufacturer = currentFilament.manufacturer;
              profile_loaded = true;
              xSemaphoreGive(stateMutex);

              Serial.print("[FILAMENT] Загружен через BLE: ");
              Serial.print(currentFilament.material);
              Serial.print(" (");
              Serial.print(currentFilament.manufacturer);
              Serial.println(")");

              // Переход в STATE_RUNNING если есть катушка на весах
              if (currentWeight > MIN_SPOOL_WEIGHT) {
                transitionToState(STATE_RUNNING);
                Serial.println("[UI] Профиль загружен через BLE, переход в RUNNING");
                load_screen(6);
              } else {
                Serial.println("[UI] Профиль загружен, но катушка не обнаружена");
              }

              saveLastFilament();
            } else {
              Serial.println("[BLE] ОШИБКА: Не удалось получить mutex");
            }
          }
        }
      }
      // Команда GET_PROFILE_LIST (отправить список всех профилей)
      else if (value.indexOf("\"cmd\":\"get_profile_list\"") >= 0) {
        Serial.println("[BLE] Запрос списка профилей");
        sendProfileList();
      }
      // Команда GET_PROFILE (отправить полный профиль по ID)
      else if (value.indexOf("\"cmd\":\"get_profile\"") >= 0) {
        int idx = value.indexOf("\"id\":\"");
        if (idx >= 0) {
          idx += 6;
          int endIdx = value.indexOf("\"", idx);
          if (endIdx > idx) {
            String id = value.substring(idx, endIdx);
            Serial.printf("[BLE] Запрос профиля: %s\n", id.c_str());
            sendFullProfile(id);
          }
        }
      }
      // Команда ADD_PROFILE (добавить новый профиль в базу)
      else if (value.indexOf("\"cmd\":\"add_profile\"") >= 0) {
        Serial.println("[BLE] Добавление нового профиля");
        
        int dataIdx = value.indexOf("\"data\":{");
        if (dataIdx >= 0) {
          int startIdx = value.indexOf("{", dataIdx + 7);
          int endIdx = value.lastIndexOf("}");
          
          if (startIdx >= 0 && endIdx > startIdx) {
            String profileData = value.substring(startIdx, endIdx + 1);
            bool success = addProfileToDatabase(profileData);
            
            // Отправляем подтверждение
            String response = "{\"cmd\":\"add_profile_response\",\"success\":";
            response += success ? "true" : "false";
            response += "}";
            pDbSyncChar->setValue(response.c_str());
            pDbSyncChar->notify();
            
            Serial.printf("[DB] Профиль %s\n", success ? "добавлен" : "не добавлен");
          }
        }
      }
    }
  };
  
  static MyCharCallbacks charCallbacks;
  pCommandChar->setCallbacks(&charCallbacks);
  
  pService->start();
  
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  
  Serial.printf("✓ (%s)\n", DEVICE_NAME);
  
  // Загрузка последнего профиля
  // loadLastFilament();
  
  // Определяем начальное состояние - НЕ ПЕРЕХОДИМ В RUNNING АВТОМАТИЧЕСКИ
  delay(1000);
  profile_loaded = false;  // ВАЖНО: сбрасываем!

  if (currentWeight > MIN_SPOOL_WEIGHT) {
      currentState = STATE_SELECT_METHOD;
      load_screen(3);
      Serial.println("[INIT] Катушка обнаружена, показываем выбор метода");
  } else {
      currentState = STATE_WAIT_SPOOL;
      load_screen(2);
      Serial.println("[INIT] Ожидание катушки");
  }
  
  Serial.println("\n╔════════════════════════════════════╗");
  Serial.println("║  ГОТОВО!                          ║");
  Serial.println("╚════════════════════════════════════╝");
  Serial.println("\nКоманды:");
  Serial.println("  1-6    - переключить экран");
  Serial.println("  w850   - вес 850g");
  Serial.println("  t      - tare");
  Serial.println("  p75.5  - процент 75.5%");
  Serial.println("  l250   - длина 250m");
  Serial.println("  profile / noprofile");
  Serial.println("  id <filament_id> - загрузить профиль");
  Serial.println("    Пример: id 3d-fuel_pla+_1000.0_1.75");
  Serial.println();
}

// ============================================
// LOOP
// ============================================
void loop() {
  lv_tick_inc(5);  // Добавляем 5ms к таймеру LVGL
  lv_timer_handler();
  
  static unsigned long lastWeightUpdate = 0;
  static unsigned long lastNfcCheck = 0;
  static unsigned long lastTimeUpdate = 0;
  static unsigned long lastBleUpdate = 0;
  static unsigned long lastWifiCheck = 0;
  static bool profileListSent = false;    // Флаг отправки списка профилей
  
  unsigned long now = millis();
  
  // ИСПРАВЛЕНИЕ: WiFi reconnect каждые 30 секунд
  if (now - lastWifiCheck > 30000) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Переподключение...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      
      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(250);
        attempts++;
      }
      
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Переподключено! IP: %s\n", WiFi.localIP().toString().c_str());
      } else {
        Serial.println("[WiFi] Не удалось переподключиться");
      }
    }
  }
  
  // Сброс флага при отключении (profileListSent используется только для логирования)
  if (!deviceConnected && profileListSent) {
    profileListSent = false;
  }

  // Примечание: Автоматическая отправка профилей убрана.
  // Мобильное приложение само запрашивает список через get_profile_list команду.
  
  // Обновление веса (каждые WEIGHT_UPDATE_INTERVAL ms)
  if (now - lastWeightUpdate > WEIGHT_UPDATE_INTERVAL) {
    lastWeightUpdate = now;
    updateWeightSensor();
  }
  
  // Проверка NFC (каждые NFC_CHECK_INTERVAL ms)
  if (now - lastNfcCheck > NFC_CHECK_INTERVAL) {
    lastNfcCheck = now;
    checkNFC();
  }
  
  // Обновление времени (каждые TIME_UPDATE_INTERVAL ms)
  if (now - lastTimeUpdate > TIME_UPDATE_INTERVAL) {
    lastTimeUpdate = now;
    update_time();
  }
  
  // BLE отправка данных (каждые BLE_UPDATE_INTERVAL ms)
  if (deviceConnected && now - lastBleUpdate > BLE_UPDATE_INTERVAL) {
    lastBleUpdate = now;
    sendBLEData();
  }
  
  // HTTP сервер
  server.handleClient();
  
  // Serial команды
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    
    Serial.print("Получена команда: [");
    Serial.print(cmd);
    Serial.println("]");
    
    if (cmd == "profile") {
      profile_loaded = true;
      Serial.println("→ Профиль загружен");
      if (current_screen == 6) {
        update_material_and_manufacturer();
        update_percent(current_percent);
        update_length(current_length);
      }
    }
    else if (cmd == "noprofile") {
      profile_loaded = false;
      Serial.println("→ Профиль убран (показываем '?')");
      if (current_screen == 6) {
        update_material_and_manufacturer();
        update_percent(current_percent);
        update_length(current_length);
      }
    }
    else if (cmd.length() == 1 && cmd[0] >= '1' && cmd[0] <= '6') {
      int screen = cmd[0] - '0';
      load_screen(screen);
      Serial.printf("→ Экран %d загружен\n", screen);
    }
    else if (cmd.startsWith("w")) {
      float weight = cmd.substring(1).toFloat();
      currentWeight = weight;
      update_weight(weight);
      Serial.printf("→ Вес обновлен: %.0fg\n", weight);
    }
    else if (cmd == "t") {
      currentWeight = 0;
      update_weight(0);
      Serial.println("→ Tare");
    }
    else if (cmd.startsWith("p")) {
      float percent = cmd.substring(1).toFloat();
      update_percent(percent);
      Serial.printf("→ Процент обновлен: %.1f%%\n", percent);
    }
    else if (cmd.startsWith("l")) {
      int length = cmd.substring(1).toInt();
      update_length(length);
      Serial.printf("→ Длина обновлена: %dm\n", length);
    }
    else if (cmd.startsWith("id ")) {
      // Загрузка профиля по ID: id 3d-fuel_pla+_1000.0_1.75
      String filamentId = cmd.substring(3);
      filamentId.trim();
      Serial.printf("→ Загрузка профиля: %s\n", filamentId.c_str());
      loadFilamentProfile(filamentId);
      
      if (profile_loaded) {
        // Переходим на экран 6 и обновляем данные
        currentState = STATE_RUNNING;
        load_screen(6);
        
        if (currentFilament.weight > 0) {
          // weight - это вес чистого филамента (без катушки), spool_weight - вес пустой катушки
          // netWeight = currentWeight - spool_weight (уже вычислено)
          // percent = netWeight / weight * 100
          float percent = (netWeight / currentFilament.weight) * 100.0f;
          if (percent > 100) percent = 100;
          if (percent < 0) percent = 0;
          current_percent = percent;
          update_percent(percent);
          
          if (currentFilament.diameter > 0 && currentFilament.density > 0) {
            int length = (int)calculateLength(netWeight, currentFilament.diameter, currentFilament.density);
            current_length = length;
            update_length(length);
          }
        }
        
        update_material_and_manufacturer();
        Serial.println("→ Profile loaded");
      } else {
        Serial.println("→ Профиль не найден!");
      }
    }
    else {
      Serial.println("→ Неизвестная команда!");
    }
  }
  
  delay(5);
}

// ============================================
// CALLBACK ДИСПЛЕЯ (LVGL v9)
// ============================================
void my_disp_flush(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  
  lv_draw_sw_rgb565_swap(px_map, w * h);
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  
  lv_display_flush_ready(disp_drv);
}

// ============================================
// СОЗДАНИЕ ВСЕХ ЭКРАНОВ
// ============================================
void create_all_screens() {
  
  // ========== 1. BOOT SCREEN ==========
  screen_boot = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screen_boot, lv_color_black(), 0);
  
  lv_obj_t* label_boot = lv_label_create(screen_boot);
  lv_label_set_text(label_boot, "FD-01");
  lv_obj_set_style_text_font(label_boot, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(label_boot, lv_color_white(), 0);
  lv_obj_center(label_boot);
  
  lv_obj_t* label_loading = lv_label_create(screen_boot);
  lv_label_set_text(label_loading, "Loading...");
  lv_obj_set_style_text_color(label_loading, lv_color_hex(0x808080), 0);
  lv_obj_align(label_loading, LV_ALIGN_CENTER, 0, 50);
  
  // ========== 2. WAIT SPOOL SCREEN (упрощенный для круглого дисплея) ==========
  screen_wait_spool = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screen_wait_spool, lv_color_black(), 0);
  
  // Заголовок
  lv_obj_t* label_wait_title = lv_label_create(screen_wait_spool);
  lv_label_set_text(label_wait_title, "FD-01");
  lv_obj_set_style_text_font(label_wait_title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label_wait_title, lv_color_white(), 0);
  lv_obj_align(label_wait_title, LV_ALIGN_TOP_MID, 0, 20);
  
  // Сообщение (центр)
  lv_obj_t* label_wait_msg = lv_label_create(screen_wait_spool);
  lv_label_set_text(label_wait_msg, "Place spool\non holder");
  lv_obj_set_style_text_font(label_wait_msg, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label_wait_msg, lv_color_hex(0xFFFF00), 0);
  lv_obj_set_style_text_align(label_wait_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(label_wait_msg);
  
  // Подсказка внизу
  lv_obj_t* label_wait_hint = lv_label_create(screen_wait_spool);
  lv_label_set_text(label_wait_hint, "Waiting...");
  lv_obj_set_style_text_font(label_wait_hint, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(label_wait_hint, lv_color_hex(0x808080), 0);
  lv_obj_align(label_wait_hint, LV_ALIGN_BOTTOM_MID, 0, -30);
  
  // ========== 3. SELECT METHOD SCREEN (компактный для круглого дисплея) ==========
  screen_select_method = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screen_select_method, lv_color_black(), 0);
  
  // Заголовок
  lv_obj_t* label_select_title = lv_label_create(screen_select_method);
  lv_label_set_text(label_select_title, "Select Method");
  lv_obj_set_style_text_font(label_select_title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(label_select_title, lv_color_white(), 0);
  lv_obj_align(label_select_title, LV_ALIGN_TOP_MID, 0, 20);
  
  // Кнопка NFC (голубая, компактная)
  lv_obj_t* btn_nfc = lv_button_create(screen_select_method);
  lv_obj_set_size(btn_nfc, 180, 60);
  lv_obj_align(btn_nfc, LV_ALIGN_CENTER, 0, -30);
  lv_obj_set_style_bg_color(btn_nfc, lv_color_hex(0x00FFFF), 0);
  lv_obj_set_style_radius(btn_nfc, 10, 0);
  lv_obj_add_event_cb(btn_nfc, btn_nfc_event, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t* label_nfc = lv_label_create(btn_nfc);
  lv_label_set_text(label_nfc, "NFC TAG");
  lv_obj_set_style_text_font(label_nfc, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(label_nfc, lv_color_black(), 0);
  lv_obj_center(label_nfc);
  
  // Кнопка APP (зеленая, компактная)
  lv_obj_t* btn_app = lv_button_create(screen_select_method);
  lv_obj_set_size(btn_app, 180, 60);
  lv_obj_align(btn_app, LV_ALIGN_CENTER, 0, 40);
  lv_obj_set_style_bg_color(btn_app, lv_color_hex(0x00FF00), 0);
  lv_obj_set_style_radius(btn_app, 10, 0);
  lv_obj_add_event_cb(btn_app, btn_app_event, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t* label_app = lv_label_create(btn_app);
  lv_label_set_text(label_app, "MOBILE APP");
  lv_obj_set_style_text_font(label_app, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(label_app, lv_color_black(), 0);
  lv_obj_center(label_app);
  
  // ========== 4. WAIT NFC SCREEN (упрощенный + кнопка возврата) ==========
  screen_wait_nfc = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screen_wait_nfc, lv_color_black(), 0);
  
  // Заголовок
  lv_obj_t* label_nfc_title = lv_label_create(screen_wait_nfc);
  lv_label_set_text(label_nfc_title, "NFC Mode");
  lv_obj_set_style_text_font(label_nfc_title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label_nfc_title, lv_color_white(), 0);
  lv_obj_align(label_nfc_title, LV_ALIGN_TOP_MID, 0, 20);
  
  // Сообщение (центр, выше)
  lv_obj_t* label_nfc_msg = lv_label_create(screen_wait_nfc);
  lv_label_set_text(label_nfc_msg, "Scan NFC tag");
  lv_obj_set_style_text_font(label_nfc_msg, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label_nfc_msg, lv_color_hex(0x00FFFF), 0);
  lv_obj_align(label_nfc_msg, LV_ALIGN_CENTER, 0, -20);
  
  // Подсказка (выше)
  lv_obj_t* label_nfc_hint = lv_label_create(screen_wait_nfc);
  lv_label_set_text(label_nfc_hint, "Place tag near reader");
  lv_obj_set_style_text_font(label_nfc_hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(label_nfc_hint, lv_color_hex(0x808080), 0);
  lv_obj_align(label_nfc_hint, LV_ALIGN_CENTER, 0, 20);
  
  // Кнопка возврата (красный крестик, выше)
  lv_obj_t* btn_back_nfc = lv_button_create(screen_wait_nfc);
  lv_obj_set_size(btn_back_nfc, 50, 50);
  lv_obj_align(btn_back_nfc, LV_ALIGN_BOTTOM_MID, 0, -30);
  lv_obj_set_style_bg_color(btn_back_nfc, lv_color_hex(0xFF0000), 0);
  lv_obj_set_style_radius(btn_back_nfc, 25, 0);
  lv_obj_add_event_cb(btn_back_nfc, btn_back_event, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t* label_back_nfc = lv_label_create(btn_back_nfc);
  lv_label_set_text(label_back_nfc, "X");
  lv_obj_set_style_text_font(label_back_nfc, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label_back_nfc, lv_color_white(), 0);
  lv_obj_center(label_back_nfc);
  
  // ========== 5. WAIT APP SCREEN (упрощенный + кнопка возврата, без WiFi) ==========
  screen_wait_app = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screen_wait_app, lv_color_black(), 0);
  
  // Заголовок
  lv_obj_t* label_app_title = lv_label_create(screen_wait_app);
  lv_label_set_text(label_app_title, "App Mode");
  lv_obj_set_style_text_font(label_app_title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label_app_title, lv_color_white(), 0);
  lv_obj_align(label_app_title, LV_ALIGN_TOP_MID, 0, 20);
  
  // Сообщение (центр, выше)
  lv_obj_t* label_app_msg = lv_label_create(screen_wait_app);
  lv_label_set_text(label_app_msg, "Waiting for\nBLE connection");
  lv_obj_set_style_text_font(label_app_msg, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(label_app_msg, lv_color_hex(0x00FF00), 0);
  lv_obj_set_style_text_align(label_app_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(label_app_msg, LV_ALIGN_CENTER, 0, -20);
  
  // Подсказка (выше)
  lv_obj_t* label_app_hint = lv_label_create(screen_wait_app);
  lv_label_set_text(label_app_hint, "Open mobile app");
  lv_obj_set_style_text_font(label_app_hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(label_app_hint, lv_color_hex(0x808080), 0);
  lv_obj_align(label_app_hint, LV_ALIGN_CENTER, 0, 20);
  
  // Кнопка возврата (красный крестик, выше)
  lv_obj_t* btn_back_app = lv_button_create(screen_wait_app);
  lv_obj_set_size(btn_back_app, 50, 50);
  lv_obj_align(btn_back_app, LV_ALIGN_BOTTOM_MID, 0, -30);
  lv_obj_set_style_bg_color(btn_back_app, lv_color_hex(0xFF0000), 0);
  lv_obj_set_style_radius(btn_back_app, 25, 0);
  lv_obj_add_event_cb(btn_back_app, btn_back_event, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t* label_back_app = lv_label_create(btn_back_app);
  lv_label_set_text(label_back_app, "X");
  lv_obj_set_style_text_font(label_back_app, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label_back_app, lv_color_white(), 0);
  lv_obj_center(label_back_app);
  
  // ========== 6. RUNNING SCREEN (НОВЫЙ ДИЗАЙН) ==========
  screen_running = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(screen_running, lv_color_black(), 0);
  
  // ПРОГРЕСС-БАР ПО КОНТУРУ (незамкнутый круг снизу)
  arc_progress = lv_arc_create(screen_running);
  lv_obj_set_size(arc_progress, 220, 220);
  lv_obj_center(arc_progress);
  lv_arc_set_rotation(arc_progress, 135);  // Начало слева-снизу
  lv_arc_set_bg_angles(arc_progress, 0, 270);  // 270° дуга (разрыв 90° снизу)
  lv_arc_set_range(arc_progress, 0, 270);  // Диапазон значений 0-270
  lv_obj_set_style_arc_color(arc_progress, lv_color_hex(0x404040), LV_PART_MAIN);  // Серый фон
  lv_obj_set_style_arc_color(arc_progress, lv_color_white(), LV_PART_INDICATOR);   // Белый прогресс
  lv_obj_set_style_arc_width(arc_progress, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc_progress, 12, LV_PART_INDICATOR);
  lv_obj_remove_style(arc_progress, NULL, LV_PART_KNOB);
  lv_arc_set_value(arc_progress, (int)(current_percent * 270.0 / 100.0));  // Масштабируем на 270°
  
  // МАТЕРИАЛ (большими буквами сверху, по центру)
  label_material = lv_label_create(screen_running);
  lv_label_set_text(label_material, current_material.c_str());
  lv_obj_set_style_text_font(label_material, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label_material, lv_color_white(), 0);
  lv_obj_align(label_material, LV_ALIGN_CENTER, 0, -70);

  // ПРОИЗВОДИТЕЛЬ (чуть ниже, по центру)
  label_manufacturer = lv_label_create(screen_running);
  lv_label_set_text(label_manufacturer, current_manufacturer.c_str());
  lv_obj_set_style_text_font(label_manufacturer, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(label_manufacturer, lv_color_hex(0x808080), 0);
  lv_obj_align(label_manufacturer, LV_ALIGN_CENTER, 0, -48);

  // ПРОЦЕНТ - основная цифра (большая, левее)
  label_percent_main = lv_label_create(screen_running);
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", (int)current_percent);
  lv_label_set_text(label_percent_main, buf);
  lv_obj_set_style_text_font(label_percent_main, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(label_percent_main, lv_color_white(), 0);
  lv_obj_align(label_percent_main, LV_ALIGN_CENTER, -60, -5);

  // ПРОЦЕНТ - знак % (ближе к цифре, желтый, левее)
  label_percent_sign = lv_label_create(screen_running);
  lv_label_set_text(label_percent_sign, "%");
  lv_obj_set_style_text_font(label_percent_sign, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label_percent_sign, lv_color_hex(0xFFFF00), 0);
  lv_obj_align(label_percent_sign, LV_ALIGN_CENTER, -15, -15);

  // ПРОЦЕНТ - десятичная часть (под знаком %, ближе, левее)
  label_percent_decimal = lv_label_create(screen_running);
  int decimal = (int)((current_percent - (int)current_percent) * 10);
  snprintf(buf, sizeof(buf), ".%d", decimal);
  lv_label_set_text(label_percent_decimal, buf);
  lv_obj_set_style_text_font(label_percent_decimal, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(label_percent_decimal, lv_color_white(), 0);
  lv_obj_align(label_percent_decimal, LV_ALIGN_CENTER, -15, 10);
  
  // WGT: (желтый, выше, крупнее, левее)
  label_weight_title = lv_label_create(screen_running);
  lv_label_set_text(label_weight_title, "WGT");
  lv_obj_set_style_text_font(label_weight_title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(label_weight_title, lv_color_hex(0xFFFF00), 0);
  lv_obj_align(label_weight_title, LV_ALIGN_CENTER, 20, 0);
  
  // Вес (белый, меньше, левее)
  label_weight_value = lv_label_create(screen_running);
  snprintf(buf, sizeof(buf), "%dg", (int)currentWeight);
  lv_label_set_text(label_weight_value, buf);
  lv_obj_set_style_text_font(label_weight_value, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(label_weight_value, lv_color_white(), 0);
  lv_obj_align(label_weight_value, LV_ALIGN_CENTER, 65, 0);
  
  // LGT: (желтый, выше, крупнее, левее)
  label_length_title = lv_label_create(screen_running);
  lv_label_set_text(label_length_title, "LGT");
  lv_obj_set_style_text_font(label_length_title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(label_length_title, lv_color_hex(0xFFFF00), 0);
  lv_obj_align(label_length_title, LV_ALIGN_CENTER, 20, 25);
  
  // Длина (белый, меньше, левее)
  label_length_value = lv_label_create(screen_running);
  snprintf(buf, sizeof(buf), "%dm", current_length);
  lv_label_set_text(label_length_value, buf);
  lv_obj_set_style_text_font(label_length_value, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(label_length_value, lv_color_white(), 0);
  lv_obj_align(label_length_value, LV_ALIGN_CENTER, 65, 25);

  // ВРЕМЯ (внизу по центру, МСК)
  label_time = lv_label_create(screen_running);
  lv_label_set_text(label_time, "--:--");
  lv_obj_set_style_text_font(label_time, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(label_time, lv_color_hex(0x808080), 0);
  lv_obj_align(label_time, LV_ALIGN_BOTTOM_MID, 0, -30);
}

// ============================================
// ПЕРЕКЛЮЧЕНИЕ ЭКРАНОВ
// ============================================
void load_screen(int screen_num) {
  current_screen = screen_num;
  
  switch(screen_num) {
    case 1: 
      lv_scr_load(screen_boot); 
      break;
    case 2: 
      lv_scr_load(screen_wait_spool); 
      break;
    case 3: 
      lv_scr_load(screen_select_method); 
      break;
    case 4: 
      lv_scr_load(screen_wait_nfc); 
      break;
    case 5: 
      lv_scr_load(screen_wait_app); 
      break;
    case 6:
      lv_scr_load(screen_running);
      // Обновляем все виджеты при загрузке экрана
      update_material_and_manufacturer();  // ВАЖНО: обновляем материал/производителя
      update_weight(currentWeight);
      update_percent(current_percent);
      update_length(current_length);
      update_time();  // Обновляем время
      break;
  }
  
  // НЕ вызываем lv_refr_now() - LVGL обновляется автоматически в loop()
}

// ============================================
// ОБНОВЛЕНИЕ ВЕСА
// ============================================
void update_weight(float weight) {
  currentWeight = weight;

  // Вычисляем чистый вес (вес филамента без катушки)
  float displayWeight = netWeight; // Показываем чистый вес филамента

  if (current_screen == 6) {  // Только если на экране RUNNING
    if (label_weight_value == NULL) return;

    char buf[32];
    if (displayWeight >= 1000) {
      snprintf(buf, sizeof(buf), "%.1fkg", displayWeight / 1000.0);
    } else {
      snprintf(buf, sizeof(buf), "%dg", (int)displayWeight);
    }
    lv_label_set_text(label_weight_value, buf);
  }
  // Не выводим debug для неактивного экрана - слишком частое обновление
}

// ============================================
// ОБНОВЛЕНИЕ ПРОЦЕНТА
// ============================================
void update_percent(float percent) {
  current_percent = percent;

  if (current_screen == 6) {
    // Обновляем прогресс-бар (масштабируем на 270°)
    int arc_value = (int)(percent * 270.0 / 100.0);
    lv_arc_set_value(arc_progress, arc_value);

    // Основная цифра
    char buf[16];
    if (percent >= 100) {
      snprintf(buf, sizeof(buf), "%d", (int)percent);
      lv_label_set_text(label_percent_main, buf);
      lv_obj_set_style_text_font(label_percent_main, &lv_font_montserrat_32, 0);
    } else {
      snprintf(buf, sizeof(buf), "%d", (int)percent);
      lv_label_set_text(label_percent_main, buf);
      lv_obj_set_style_text_font(label_percent_main, &lv_font_montserrat_48, 0);
    }

    // Десятичная часть
    int decimal = (int)((percent - (int)percent) * 10);
    snprintf(buf, sizeof(buf), ".%d", decimal);
    lv_label_set_text(label_percent_decimal, buf);
  }
}

// ============================================
// ОБНОВЛЕНИЕ ДЛИНЫ
// ============================================
void update_length(int length) {
  current_length = length;

  if (current_screen == 6) {
    char buf[32];
    if (length >= 1000) {
      snprintf(buf, sizeof(buf), "%.1fkm", length / 1000.0);
    } else {
      snprintf(buf, sizeof(buf), "%dm", length);
    }
    lv_label_set_text(label_length_value, buf);
  }
}

// ============================================
// ОБНОВЛЕНИЕ МАТЕРИАЛА И ПРОИЗВОДИТЕЛЯ
// ============================================
void update_material_and_manufacturer() {
  if (current_screen == 6) {
    lv_label_set_text(label_material, current_material.c_str());
    lv_label_set_text(label_manufacturer, current_manufacturer.c_str());
  }
}

// ============================================
// ОБНОВЛЕНИЕ ВРЕМЕНИ
// ============================================
void update_time() {
  struct tm timeinfo;
  char buf[32];

  // getLocalTime с таймаутом 100мс
  if (getLocalTime(&timeinfo, 100)) {
    // Реальное время по МСК (NTP уже настроен на UTC+3)
    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  } else {
    // Если NTP не синхронизировано, показываем "--:--"
    snprintf(buf, sizeof(buf), "--:--");

    // Debug: сообщаем о проблеме только каждые 30 секунд
    static unsigned long lastNtpWarn = 0;
    if (millis() - lastNtpWarn > 30000) {
      lastNtpWarn = millis();
      Serial.println("[NTP] Время не синхронизировано");
    }
  }

  if (current_screen == 6) {
    lv_label_set_text(label_time, buf);
  }
}

// ============================================
// ОБРАБОТЧИКИ КНОПОК
// ============================================
void btn_nfc_event(lv_event_t* e) {
  Serial.println("→ Выбран NFC");
  if (transitionToState(STATE_WAIT_NFC)) {
    load_screen(4);
  }
}

void btn_app_event(lv_event_t* e) {
  Serial.println("→ Выбран APP");
  if (transitionToState(STATE_WAIT_APP)) {
    load_screen(5);
  }
}

void btn_back_event(lv_event_t* e) {
  Serial.println("→ Возврат к выбору метода");
  if (transitionToState(STATE_SELECT_METHOD)) {
    load_screen(3);
  }
}

// ============================================
// ФУНКЦИИ РАБОТЫ С МОДУЛЯМИ
// ============================================

// Обновление веса с тензодатчика
void updateWeightSensor() {
  #if DEBUG_MODE
  // В DEBUG режиме вес устанавливается через Serial команды
  // Ничего не делаем здесь
  #else
  if (scale.is_ready()) {
    currentWeight = scale.get_units(3);
  }
  #endif
  
  // Вычисляем чистый вес (без катушки)
  if (profile_loaded && currentFilament.spool_weight > 0) {
    netWeight = currentWeight - currentFilament.spool_weight;
    if (netWeight < 0) netWeight = 0;
  } else {
    netWeight = currentWeight;
  }
  
  // Автоматический переход из STATE_WAIT_SPOOL в SELECT_METHOD
  if (currentState == STATE_WAIT_SPOOL && currentWeight > MIN_SPOOL_WEIGHT) {
    if (transitionToState(STATE_SELECT_METHOD)) {
      Serial.println("[UI] Катушка обнаружена, показываем выбор метода");
      load_screen(3);
    }
  }

  // Автоматический возврат из SELECT_METHOD в WAIT_SPOOL если вес убрали
  if (currentState == STATE_SELECT_METHOD && currentWeight < MIN_SPOOL_WEIGHT) {
    if (transitionToState(STATE_WAIT_SPOOL)) {
      Serial.println("[UI] Катушка убрана, возврат к ожиданию");
      load_screen(2);
    }
  }

  // Возврат из WAIT_NFC/WAIT_APP в WAIT_SPOOL если вес убрали
  if ((currentState == STATE_WAIT_NFC || currentState == STATE_WAIT_APP) && currentWeight < MIN_SPOOL_WEIGHT) {
    if (transitionToState(STATE_WAIT_SPOOL)) {
      Serial.println("[UI] Катушка убрана во время ожидания, возврат");
      load_screen(2);
    }
  }

  // Возврат из RUNNING в WAIT_SPOOL если катушку убрали
  if (currentState == STATE_RUNNING && currentWeight < MIN_SPOOL_WEIGHT) {
    if (transitionToState(STATE_WAIT_SPOOL)) {
      Serial.println("[UI] Катушка убрана из RUNNING, возврат к ожиданию");
      load_screen(2);
    }
  }
  
  if (current_screen == 6) {
    update_weight(currentWeight);
    
    if (profile_loaded && currentFilament.weight > 0) {
      // weight - это вес чистого филамента (без катушки)
      // netWeight = currentWeight - spool_weight (уже вычислено)
      float percent = (netWeight / currentFilament.weight) * 100.0f;
      if (percent > 100) percent = 100;
      if (percent < 0) percent = 0;
      update_percent(percent);
      
      if (currentFilament.diameter > 0 && currentFilament.density > 0) {
        int length = (int)calculateLength(netWeight, currentFilament.diameter, currentFilament.density);
        update_length(length);
      }
    }
  }
}

// Проверка NFC карты
void checkNFC() {
  // Проверяем NFC только когда пользователь выбрал NFC метод
  if (currentState != STATE_WAIT_NFC) {
    return;
  }

  // Debug счётчик для отслеживания работы
  static unsigned long nfcCheckCount = 0;
  nfcCheckCount++;

  // Выводим debug каждые 25 проверок (~5 секунд при 200мс интервале)
  if (nfcCheckCount % 25 == 0) {
    Serial.printf("[NFC] Сканирование... (проверка #%lu)\n", nfcCheckCount);
  }

  // Захватываем SPI mutex с таймаутом
  if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    Serial.println("[NFC] SPI mutex занят, пропускаем");
    return;
  }

  // Переключаем SPI на NFC
  SPI.end();
  SPI.begin(LCD_SCK, RC522_MISO, LCD_MOSI, RC522_CS);

  // Даём время на стабилизацию SPI
  delay(5);

  // Полная инициализация RC522 (как при записи)
  rfid.PCD_Init();
  delay(5);

  // Пробуем обнаружить карту несколькими способами
  bool cardFound = false;

  // Способ 1: WakeupA - будит карту в режиме HALT
  byte atqa[2];
  byte atqaLen = sizeof(atqa);
  if (rfid.PICC_WakeupA(atqa, &atqaLen) == MFRC522::STATUS_OK) {
    cardFound = true;
  }

  // Способ 2: Стандартное обнаружение новой карты
  if (!cardFound && rfid.PICC_IsNewCardPresent()) {
    cardFound = true;
  }

  bool newCard = cardFound;

  if (!newCard) {
    // Нет карты - освобождаем SPI и выходим
    SPI.end();
    SPI.begin(LCD_SCK, -1, LCD_MOSI, LCD_CS);
    SPI.setDataMode(SPI_MODE0);
    SPI.setBitOrder(MSBFIRST);
    SPI.setFrequency(40000000);
    xSemaphoreGive(spiMutex);
    return;
  }

  Serial.println("[NFC] Обнаружена карта!");

  if (rfid.PICC_ReadCardSerial()) {
    String uid = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      if (rfid.uid.uidByte[i] < 0x10) uid += "0";
      uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    Serial.printf("[NFC] UID: %s\n", uid.c_str());

    if (uid != lastNfcUid) {
      lastNfcUid = uid;

      String filamentId = readNfcData();
      Serial.printf("[NFC] Прочитанный ID: '%s' (длина: %d)\n", filamentId.c_str(), filamentId.length());

      if (filamentId.length() > 0) {
        loadFilamentProfile(filamentId);
        // loadFilamentProfile() уже делает переход в STATE_RUNNING и load_screen(6)
        // Не дублируем логику здесь
        if (!profile_loaded) {
          Serial.println("[NFC] Профиль не найден в базе данных");
        }
        lastNfcUid = "";
      } else {
        Serial.println("[NFC] Не удалось прочитать данные с карты");
        lastNfcUid = "";
      }
    } else {
      Serial.println("[NFC] Карта уже была прочитана");
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  } else {
    Serial.println("[NFC] Не удалось прочитать серийный номер карты");
  }

  // Возвращаем SPI на дисплей
  SPI.end();
  SPI.begin(LCD_SCK, -1, LCD_MOSI, LCD_CS);
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);
  SPI.setFrequency(40000000);

  // Освобождаем SPI mutex
  xSemaphoreGive(spiMutex);
}

// Проверка типа NFC карты
bool isNtagCard() {
  MFRC522::PICC_Type piccType = rfid.PICC_GetType(rfid.uid.sak);
  return (piccType == MFRC522::PICC_TYPE_MIFARE_UL);  // NTAG определяется как Ultralight
}

// Общая функция парсинга NDEF payload из буфера
String parseNdefPayload(byte* buffer, byte bufferSize) {
  String data = "";
  
  // Проверяем NDEF формат (TLV: 03 = NDEF Message, D1 = Record Header, 54 = Type 'T')
  if (buffer[0] == 0x03 && buffer[2] == 0xD1 && buffer[5] == 0x54) {
    // NDEF Text Record формат
    byte langLength = buffer[6] & 0x3F;  // Длина языкового кода
    byte payloadStart = 7 + langLength;  // Начало текстовых данных
    
    // Извлекаем текст до конца буфера или терминатора
    for (byte i = payloadStart; i < bufferSize && buffer[i] != 0x00 && buffer[i] != 0xFE; i++) {
      if (buffer[i] >= 32 && buffer[i] <= 126) {  // Только печатные ASCII символы
        data += (char)buffer[i];
      }
    }
  } else {
    // Raw формат - просто читаем текст
    for (byte i = 0; i < bufferSize && buffer[i] != 0x00; i++) {
      if (buffer[i] >= 32 && buffer[i] <= 126) {
        data += (char)buffer[i];
      }
    }
  }
  
  return data;
}

// Чтение данных с NTAG (Type 2 Tag)
String readNtagData() {
  // NTAG не требует аутентификации для чтения
  // Данные начинаются со страницы 4, каждая страница = 4 байта
  byte buffer[18];
  byte size = sizeof(buffer);

  // Читаем страницы 4-7 (16 байт) - там начинается NDEF
  MFRC522::StatusCode status = rfid.MIFARE_Read(4, buffer, &size);
  if (status != MFRC522::STATUS_OK) {
    Serial.print("[NFC] NTAG ошибка чтения: ");
    Serial.println(rfid.GetStatusCodeName(status));
    return "";
  }

  // Парсим первый блок
  String data = parseNdefPayload(buffer, 16);

  // Читаем следующие страницы если данных мало
  if (data.length() < 10) {
    byte buffer2[18];
    byte size2 = sizeof(buffer2);
    status = rfid.MIFARE_Read(8, buffer2, &size2);  // Страницы 8-11
    if (status == MFRC522::STATUS_OK) {
      for (byte i = 0; i < 16 && buffer2[i] != 0x00 && buffer2[i] != 0xFE; i++) {
        if (buffer2[i] >= 32 && buffer2[i] <= 126) {
          data += (char)buffer2[i];
        }
      }
    }
  }

  data.trim();
  Serial.printf("[NFC] NTAG payload: %s\n", data.c_str());
  return data;
}

// Чтение данных с MIFARE Classic
String readMifareData() {
  // MIFARE требует аутентификации
  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }

  byte trailerBlock = 7;
  MFRC522::StatusCode status = rfid.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A,
    trailerBlock,
    &key,
    &(rfid.uid)
  );

  if (status != MFRC522::STATUS_OK) {
    Serial.print("[NFC] MIFARE ошибка аутентификации: ");
    Serial.println(rfid.GetStatusCodeName(status));
    return "";
  }

  byte buffer[18];
  byte size = sizeof(buffer);

  status = rfid.MIFARE_Read(4, buffer, &size);
  if (status != MFRC522::STATUS_OK) {
    Serial.print("[NFC] MIFARE ошибка чтения: ");
    Serial.println(rfid.GetStatusCodeName(status));
    return "";
  }

  // Парсим первый блок
  String data = parseNdefPayload(buffer, 16);

  // Читаем следующий блок если данных мало
  if (data.length() < 10) {
    byte buffer2[18];
    byte size2 = sizeof(buffer2);
    status = rfid.MIFARE_Read(5, buffer2, &size2);
    if (status == MFRC522::STATUS_OK) {
      for (byte i = 0; i < 16 && buffer2[i] != 0x00 && buffer2[i] != 0xFE; i++) {
        if (buffer2[i] >= 32 && buffer2[i] <= 126) {
          data += (char)buffer2[i];
        }
      }
    }
  }

  data.trim();
  Serial.printf("[NFC] MIFARE payload: %s\n", data.c_str());
  return data;
}

// Чтение данных с NFC карты (поддержка MIFARE Classic и NTAG)
String readNfcData() {
  if (isNtagCard()) {
    Serial.println("[NFC] Обнаружен NTAG/Ultralight");
    return readNtagData();
  } else {
    Serial.println("[NFC] Обнаружен MIFARE Classic");
    return readMifareData();
  }
}

// Загрузка профиля филамента из LittleFS
void loadFilamentProfile(String filamentId) {
  File file = LittleFS.open("/filaments.json", "r");
  if (!file) {
    Serial.println("[LittleFS] Не удалось открыть filaments.json");
    profile_loaded = false;
    return;
  }
  
  String line;
  bool found = false;
  
  while (file.available()) {
    line = file.readStringUntil('\n');
    if (line.indexOf(filamentId) >= 0) {
      found = true;
      break;
    }
  }
  file.close();
  
  if (!found) {
    Serial.println("[FILAMENT] Профиль не найден");
    profile_loaded = false;
    
    // UI FEEDBACK: Показываем сообщение пользователю
    if (currentState == STATE_WAIT_NFC) {
      Serial.println("[UI] NFC карта не распознана - профиль не найден в базе");
    }
    return;
  }
  
  // ИСПРАВЛЕНИЕ: Используем ArduinoJson вместо indexOf()
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, line);
  
  if (error) {
    Serial.print("[JSON] Ошибка парсинга: ");
    Serial.println(error.c_str());
    profile_loaded = false;
    return;
  }
  
  // Безопасное извлечение данных через ArduinoJson
  currentFilament.id = filamentId;
  currentFilament.material = doc["material"] | "?";
  currentFilament.manufacturer = doc["manufacturer"] | "?";
  currentFilament.weight = doc["weight"] | 0.0f;
  currentFilament.spool_weight = doc["spool_weight"] | doc["spoolWeight"] | 0.0f;
  currentFilament.length = doc["length"] | 0;
  currentFilament.diameter = doc["diameter"] | 1.75f;
  currentFilament.density = doc["density"] | 1.24f;
  currentFilament.bed_temp = doc["bed_temp"] | doc["bedTemp"] | 60;
  
  current_material = currentFilament.material;
  current_manufacturer = currentFilament.manufacturer;
  current_length = currentFilament.length;
  profile_loaded = true;
  
  Serial.print("[FILAMENT] Загружен: ");
  Serial.print(currentFilament.material);
  Serial.print(" (");
  Serial.print(currentFilament.manufacturer);
  Serial.println(")");
  
  // Переход в STATE_RUNNING если есть катушка на весах
  if (currentWeight > MIN_SPOOL_WEIGHT) {
    if (transitionToState(STATE_RUNNING)) {
      Serial.println("[UI] Профиль загружен, переход в RUNNING");
      load_screen(6);
    }
  } else {
    Serial.println("[UI] Профиль загружен, но катушка не обнаружена");
  }

  saveLastFilament();
}

// Сохранение последнего профиля в кэш
void saveLastFilament() {
  File file = LittleFS.open("/cache.json", "w");
  if (file) {
    file.print("{\"id\":\"");
    file.print(currentFilament.id);
    file.print("\",\"weight\":");
    file.print(currentWeight);
    file.println("}");
    file.close();
    Serial.println("[CACHE] Профиль сохранен");
  }
}

// Загрузка последнего профиля из кэша
void loadLastFilament() {
  File file = LittleFS.open("/cache.json", "r");
  if (!file) {
    Serial.println("[CACHE] Кэш не найден");
    return;
  }
  
  String content = file.readString();
  file.close();
  
  int idx = content.indexOf("\"id\":\"");
  if (idx >= 0) {
    idx += 6;
    String id = content.substring(idx, content.indexOf("\"", idx));
    if (id.length() > 0) {
      Serial.print("[CACHE] Загрузка профиля: ");
      Serial.println(id);
      loadFilamentProfile(id);
    }
  }
}

// HTTP обработчик для плагина
// Экранирование строки для JSON (защита от спецсимволов)
String escapeJsonString(const String& input) {
  String output;
  output.reserve(input.length() + 10);
  for (unsigned int i = 0; i < input.length(); i++) {
    char c = input[i];
    switch (c) {
      case '\\': output += "\\\\"; break;
      case '"':  output += "\\\""; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:   output += c; break;
    }
  }
  return output;
}

void handleData() {
  // Экранируем строки для корректного JSON
  String safeName = escapeJsonString(String(DEVICE_NAME));
  String safeId = escapeJsonString(currentFilament.id);
  String safeMaterial = escapeJsonString(currentFilament.material);
  String safeManufacturer = escapeJsonString(currentFilament.manufacturer);

  String response = "{\"name\":\"" + safeName +
                    "\",\"net\":" + String(netWeight, 1) +
                    ",\"gross\":" + String(currentWeight, 1) +
                    ",\"spool\":" + String(currentFilament.spool_weight, 1) +
                    ",\"weight\":" + String(currentFilament.weight, 1) +
                    ",\"filament_id\":\"" + safeId +
                    "\",\"material\":\"" + safeMaterial +
                    "\",\"manufacturer\":\"" + safeManufacturer +
                    "\",\"percent\":" + String(current_percent, 1) +
                    ",\"length\":" + String(current_length) +
                    ",\"diameter\":" + String(currentFilament.diameter, 2) +
                    ",\"density\":" + String(currentFilament.density, 2) +
                    ",\"bed_temp\":" + String(currentFilament.bed_temp) +
                    ",\"status\":\"" + String(profile_loaded ? "active" : "idle") +
                    "\",\"profile_loaded\":" + String(profile_loaded ? "true" : "false") +
                    "}";

  server.send(200, "application/json", response);
  Serial.println("[HTTP] Данные отправлены плагину");
}

// Отправка данных через BLE
void sendBLEData() {
  // Проверка что BLE инициализирован
  if (!pDataChar) {
    Serial.println("[BLE] ОШИБКА: pDataChar == NULL");
    return;
  }

  // Экранируем строки для корректного JSON
  String safeId = escapeJsonString(currentFilament.id);
  String safeMaterial = escapeJsonString(currentFilament.material);
  String safeManufacturer = escapeJsonString(currentFilament.manufacturer);

  String data = "{\"net\":" + String(netWeight, 1) +
                ",\"gross\":" + String(currentWeight, 1) +
                ",\"spool\":" + String(currentFilament.spool_weight, 1) +
                ",\"filament_id\":\"" + safeId +
                "\",\"material\":\"" + safeMaterial +
                "\",\"manufacturer\":\"" + safeManufacturer +
                "\",\"percent\":" + String(current_percent, 1) +
                ",\"length\":" + String(current_length) +
                ",\"diameter\":" + String(currentFilament.diameter, 2) +
                ",\"density\":" + String(currentFilament.density, 2) +
                ",\"bed_temp\":" + String(currentFilament.bed_temp) +
                ",\"status\":\"" + String(profile_loaded ? "active" : "idle") +
                "\",\"profile_loaded\":" + String(profile_loaded ? "true" : "false") +
                "}";

  pDataChar->setValue(data.c_str());
  pDataChar->notify();
}

// ============================================
// ДОПОЛНИТЕЛЬНЫЕ ФУНКЦИИ
// ============================================

// Расчет длины филамента по весу, диаметру и плотности
float calculateLength(float weight_g, float diameter_mm, float density) {
  if (diameter_mm <= 0 || density <= 0 || weight_g <= 0) return 0;
  
  // Формула: Длина (м) = Объем / Площадь_сечения
  // Объем (см³) = Вес (г) / Плотность (г/см³)
  // Площадь_сечения (см²) = π × (Диаметр/2)²
  // 1 мм = 0.1 см, поэтому диаметр_см = диаметр_мм / 10
  
  float diameter_cm = diameter_mm / 10.0;  // мм -> см
  float radius_cm = diameter_cm / 2.0;
  float area_cm2 = 3.14159265359 * radius_cm * radius_cm;  // см²
  float volume_cm3 = weight_g / density;  // см³
  float length_cm = volume_cm3 / area_cm2;  // см
  float length_m = length_cm / 100.0;  // см -> м
  
  return length_m;
}

// Восстановление SPI для LCD
void restoreLcdSpi() {
  SPI.end();
  SPI.begin(LCD_SCK, -1, LCD_MOSI, LCD_CS);
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);
  SPI.setFrequency(40000000);
}

// Общая функция формирования NDEF Text Record
// Возвращает размер сформированного сообщения
int buildNdefMessage(byte* buffer, int bufferSize, const byte* payload, int payloadLen) {
  if (bufferSize < 10 + payloadLen) return 0;  // Недостаточно места
  
  byte payloadLength = 3 + payloadLen;  // lang code (2) + lang length (1) + payload
  byte messageLength = 5 + payloadLength;  // header (5) + payload
  
  memset(buffer, 0, bufferSize);
  
  buffer[0] = 0x03;              // NDEF Message TLV
  buffer[1] = messageLength;     // Длина сообщения
  buffer[2] = 0xD1;              // Record Header (MB=1, ME=1, SR=1, TNF=0x01)
  buffer[3] = 0x01;              // Type Length = 1
  buffer[4] = payloadLength;     // Payload Length
  buffer[5] = 0x54;              // Type = 'T' (Text)
  buffer[6] = 0x02;              // Status byte: UTF-8, lang code length = 2
  buffer[7] = 0x65;              // 'e'
  buffer[8] = 0x6E;              // 'n'
  
  // Копируем payload
  for (int i = 0; i < payloadLen; i++) {
    buffer[9 + i] = payload[i];
  }
  
  buffer[9 + payloadLen] = 0xFE;  // NDEF Terminator TLV
  
  return 10 + payloadLen;  // Возвращаем общий размер
}

// Запись на NTAG (страницами по 4 байта)
bool writeNtagData(String filamentId) {
  Serial.println("[NFC] Запись на NTAG...");

  byte idBytes[48];
  int idLen = filamentId.length();
  if (idLen > 40) idLen = 40;  // Ограничение для NTAG
  filamentId.getBytes(idBytes, idLen + 1);

  // Формируем NDEF сообщение
  byte ndefMsg[48];
  int msgSize = buildNdefMessage(ndefMsg, sizeof(ndefMsg), idBytes, idLen);
  
  if (msgSize == 0) {
    Serial.println("[NFC] NTAG ошибка: слишком длинный ID");
    return false;
  }

  // NTAG пишется страницами по 4 байта начиная со страницы 4
  int numPages = (msgSize + 3) / 4;

  for (int page = 0; page < numPages && page < 12; page++) {
    byte pageData[4];
    for (int i = 0; i < 4; i++) {
      int idx = page * 4 + i;
      pageData[i] = (idx < msgSize) ? ndefMsg[idx] : 0x00;
    }

    MFRC522::StatusCode status = rfid.MIFARE_Ultralight_Write(4 + page, pageData, 4);
    if (status != MFRC522::STATUS_OK) {
      Serial.printf("[NFC] NTAG ошибка записи страницы %d: %s\n", 4 + page, rfid.GetStatusCodeName(status));
      return false;
    }
  }

  Serial.printf("[NFC] NTAG записано: %s (%d страниц)\n", filamentId.c_str(), numPages);
  return true;
}

// Запись на MIFARE Classic
bool writeMifareData(String filamentId) {
  Serial.println("[NFC] Запись на MIFARE...");

  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }

  byte trailerBlock = 7;
  MFRC522::StatusCode status = rfid.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A,
    trailerBlock,
    &key,
    &(rfid.uid)
  );

  if (status != MFRC522::STATUS_OK) {
    Serial.print("[NFC] MIFARE ошибка аутентификации: ");
    Serial.println(rfid.GetStatusCodeName(status));
    return false;
  }

  byte idBytes[33];
  int idLen = filamentId.length();
  if (idLen > 32) idLen = 32;
  filamentId.getBytes(idBytes, idLen + 1);

  // Формируем NDEF сообщение
  byte ndefMsg[48];
  int msgSize = buildNdefMessage(ndefMsg, sizeof(ndefMsg), idBytes, idLen);
  
  if (msgSize == 0) {
    Serial.println("[NFC] MIFARE ошибка: слишком длинный ID");
    return false;
  }

  // MIFARE пишется блоками по 16 байт
  // Блок 4 - первые 16 байт
  byte buffer1[16];
  memcpy(buffer1, ndefMsg, 16);

  status = rfid.MIFARE_Write(4, buffer1, 16);
  if (status != MFRC522::STATUS_OK) {
    Serial.print("[NFC] MIFARE ошибка записи блока 4: ");
    Serial.println(rfid.GetStatusCodeName(status));
    return false;
  }

  // Блок 5 - следующие 16 байт (если нужно)
  if (msgSize > 16) {
    byte buffer2[16];
    memset(buffer2, 0, 16);
    int remainingBytes = msgSize - 16;
    if (remainingBytes > 16) remainingBytes = 16;
    memcpy(buffer2, ndefMsg + 16, remainingBytes);

    status = rfid.MIFARE_Write(5, buffer2, 16);
    if (status != MFRC522::STATUS_OK) {
      Serial.print("[NFC] MIFARE ошибка записи блока 5: ");
      Serial.println(rfid.GetStatusCodeName(status));
      return false;
    }
  }

  Serial.printf("[NFC] MIFARE записано: %s\n", filamentId.c_str());
  return true;
}

// Запись данных на NFC карту (поддержка MIFARE Classic и NTAG)
bool writeNfcData(String filamentId) {
  Serial.println("[NFC] Начало записи...");

  // Захватываем SPI mutex с таймаутом 500мс
  if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    Serial.println("[NFC] ОШИБКА: SPI занят");
    return false;
  }

  // Переключаем SPI на NFC
  SPI.end();
  SPI.begin(LCD_SCK, RC522_MISO, LCD_MOSI, RC522_CS);

  // Переинициализируем MFRC522 после смены SPI
  rfid.PCD_Init();
  delay(10);

  // Сбрасываем lastNfcUid чтобы карта могла быть прочитана после записи
  lastNfcUid = "";

  // Пробуем обнаружить карту несколько раз
  bool cardFound = false;

  for (int attempt = 0; attempt < 5 && !cardFound; attempt++) {
    if (attempt > 0) {
      Serial.printf("[NFC] Попытка %d/5...\n", attempt + 1);
      delay(100);  // Пауза между попытками
    }
    
    byte atqa[2];
    byte atqaLen = sizeof(atqa);

    if (rfid.PICC_WakeupA(atqa, &atqaLen) == MFRC522::STATUS_OK) {
      cardFound = true;
      Serial.println("[NFC] Карта разбужена (WakeupA)");
    } else if (rfid.PICC_IsNewCardPresent()) {
      cardFound = true;
      Serial.println("[NFC] Обнаружена новая карта");
    }
  }

  if (!cardFound) {
    Serial.println("[NFC] Карта не обнаружена");
    restoreLcdSpi();
    xSemaphoreGive(spiMutex);
    return false;
  }

  if (!rfid.PICC_ReadCardSerial()) {
    Serial.println("[NFC] Ошибка чтения серийного номера");
    restoreLcdSpi();
    xSemaphoreGive(spiMutex);
    return false;
  }

  bool success = false;

  // Определяем тип карты и вызываем соответствующую функцию записи
  if (isNtagCard()) {
    Serial.println("[NFC] Тип карты: NTAG/Ultralight");
    success = writeNtagData(filamentId);
  } else {
    Serial.println("[NFC] Тип карты: MIFARE Classic");
    success = writeMifareData(filamentId);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  // Возвращаем SPI на LCD и освобождаем mutex
  restoreLcdSpi();
  xSemaphoreGive(spiMutex);

  return success;
}

// HTTP endpoint для статуса устройства
void handleStatus() {
  String response = "{\"device\":\"" + String(DEVICE_NAME) +
                    "\",\"uptime\":" + String(millis() / 1000) +
                    ",\"free_heap\":" + String(ESP.getFreeHeap()) +
                    ",\"ble_connected\":" + String(deviceConnected ? "true" : "false") +
                    ",\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") +
                    ",\"nfc_last_read\":\"" + lastNfcUid +
                    "\",\"profile_loaded\":" + String(profile_loaded ? "true" : "false") +
                    ",\"current_state\":" + String((int)currentState) +
                    "}";
  
  server.send(200, "application/json", response);
  Serial.println("[HTTP] Status отправлен");
}

// HTTP endpoint для тарировки
void handleTare() {
  #if !DEBUG_MODE
  scale.tare(10);
  Serial.println("[HTTP] Tare выполнен (HX711)");
  #else
  currentWeight = 0;
  Serial.println("[HTTP] Tare выполнен (DEBUG)");
  #endif
  
  String response = "{\"success\":true,\"message\":\"Tare completed\"}";
  server.send(200, "application/json", response);
}


// ============================================
// ТАЧСКРИН CST816S
// ============================================

void touch_init() {
  Wire.begin(TP_SDA, TP_SCL);
  Wire.setClock(400000);

  pinMode(TP_RST, OUTPUT);
  pinMode(TP_INT, INPUT_PULLUP);

  // Reset
  digitalWrite(TP_RST, LOW);
  delay(10);
  digitalWrite(TP_RST, HIGH);
  delay(50);

  // Check device
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  byte error = Wire.endTransmission();
  
  Serial.printf("[TOUCH] I2C check: %d (0=OK)\n", error);
  
  if (error == 0) {
    // Read chip ID
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0xA7);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)1);
    if (Wire.available()) {
      byte chipId = Wire.read();
      Serial.printf("[TOUCH] Chip ID: 0x%02X (expected 0xB5)\n", chipId);
    }
    
    // Set mode
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0xFA);  // IrqCtl
    Wire.write(0x41);  // Point mode
    Wire.endTransmission();

    Serial.printf("[TOUCH] INT pin: %d, state: %d\n", TP_INT, digitalRead(TP_INT));
    attachInterrupt(digitalPinToInterrupt(TP_INT), touch_interrupt, FALLING);
    Serial.println("[TOUCH] Interrupt attached");
  } else {
    Serial.println("[TOUCH] Device not found!");
  }
}

void touch_read() {
  Serial.println("[TOUCH] Reading...");
  
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(0x02);
  Wire.endTransmission(false);

  Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)6);
  if (Wire.available() >= 6) {
    uint8_t data[6];
    for (int i = 0; i < 6; i++) {
      data[i] = Wire.read();
    }

    Serial.printf("[TOUCH] Raw data: %02X %02X %02X %02X %02X %02X\n", 
                  data[0], data[1], data[2], data[3], data[4], data[5]);

    uint8_t points = data[0] & 0x0F;
    Serial.printf("[TOUCH] Points: %d\n", points);
    
    if (points > 0) {
      touchPoint.x = ((data[1] & 0x0F) << 8) | data[2];
      touchPoint.y = ((data[3] & 0x0F) << 8) | data[4];
      touchPoint.touched = true;

      Serial.printf("[TOUCH] Coordinates: X=%d Y=%d\n", touchPoint.x, touchPoint.y);
    } else {
      touchPoint.touched = false;
      Serial.println("[TOUCH] No touch detected");
    }
  } else {
    Serial.printf("[TOUCH] Not enough data: %d bytes\n", Wire.available());
  }
}

void my_touchpad_read(lv_indev_t *indev_drv, lv_indev_data_t *data) {
  static unsigned long lastDebug = 0;
  static unsigned long callCount = 0;
  static bool lastTouched = false;
  
  callCount++;
  
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(0x02);
  Wire.endTransmission(false);
  
  Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)6);
  
  bool touched = false;
  int16_t x = 0, y = 0;
  
  if (Wire.available() >= 6) {
    uint8_t data_raw[6];
    for (int i = 0; i < 6; i++) {
      data_raw[i] = Wire.read();
    }
    
    uint8_t points = data_raw[0] & 0x0F;
    
    if (points > 0) {
      x = ((data_raw[1] & 0x0F) << 8) | data_raw[2];
      y = ((data_raw[3] & 0x0F) << 8) | data_raw[4];
      touched = true;
      
      // Логируем только первое касание (не каждый кадр)
      if (!lastTouched) {
        Serial.printf("[TOUCH] DETECTED! X=%d Y=%d\n", x, y);
      }
    }
  }
  
  lastTouched = touched;
  
  // Периодический дебаг каждые 10 секунд (уменьшено с 5)
  if (millis() - lastDebug > 10000) {
    lastDebug = millis();
    Serial.printf("[TOUCH] Polling: %lu calls\n", callCount);
  }

  if (touched) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ============================================
// СИНХРОНИЗАЦИЯ БАЗЫ ДАННЫХ
// ============================================

// Отправка списка всех профилей
void sendProfileList() {
  // Защита от конкурентных вызовов
  if (sendingProfilesNow) {
    Serial.println("[DB] ПРОПУСК: уже идёт отправка профилей");
    return;
  }
  sendingProfilesNow = true;

  Serial.println("[DB] === Начало отправки списка профилей ===");

  // Проверка что BLE инициализирован
  if (!pDbSyncChar) {
    Serial.println("[DB] ОШИБКА: pDbSyncChar == NULL, BLE не инициализирован");
    sendingProfilesNow = false;
    return;
  }
  
  // Проверяем что LittleFS смонтирован
  if (!LittleFS.begin()) {
    Serial.println("[DB] ОШИБКА: LittleFS не смонтирован!");
    String response = "{\"cmd\":\"profile_list\",\"profiles\":[],\"error\":\"LittleFS not mounted\"}";
    pDbSyncChar->setValue(response.c_str());
    pDbSyncChar->notify();
    sendingProfilesNow = false;
    return;
  }
  
  File file = LittleFS.open("/filaments.json", "r");
  if (!file) {
    Serial.println("[DB] ОШИБКА: Не удалось открыть /filaments.json");
    Serial.println("[DB] Проверь что файл загружен через LittleFS Data Upload");
    
    // Показываем список файлов в LittleFS
    Serial.println("[DB] Файлы в LittleFS:");
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    int count = 0;
    while (f) {
      Serial.printf("  - %s (%d байт)\n", f.name(), f.size());
      f = root.openNextFile();
      count++;
    }
    if (count == 0) {
      Serial.println("  (пусто - загрузи данные через Tools -> ESP32 LittleFS Data Upload)");
    }
    
    String response = "{\"cmd\":\"profile_list\",\"profiles\":[],\"error\":\"File not found\"}";
    pDbSyncChar->setValue(response.c_str());
    pDbSyncChar->notify();
    sendingProfilesNow = false;
    return;
  }

  Serial.printf("[DB] Файл открыт, размер: %d байт\n", file.size());
  
  // ОПТИМИЗАЦИЯ: Отправляем только краткую информацию (id, manufacturer, material, weight)
  // Полные данные профиля запрашиваются отдельно через get_profile
  String response = "{\"cmd\":\"profile_list\",\"profiles\":[";
  bool first = true;
  int profileCount = 0;
  int maxProfiles = 500;

  while (file.available() && profileCount < maxProfiles) {
    String line = file.readStringUntil('\n');
    line.trim();

    if (line.length() > 0 && line.startsWith("{")) {
      // Парсим JSON для извлечения только нужных полей
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, line);

      if (!error) {
        if (!first) response += ",";

        // Формируем краткий профиль (экономия ~60% трафика)
        response += "{\"id\":\"";
        response += doc["id"].as<String>();
        response += "\",\"manufacturer\":\"";
        response += doc["manufacturer"].as<String>();
        response += "\",\"material\":\"";
        response += doc["material"].as<String>();
        response += "\",\"weight\":";
        response += String(doc["weight"].as<float>(), 0);
        response += ",\"diameter\":";
        response += String(doc["diameter"].as<float>(), 2);
        response += "}";

        first = false;
        profileCount++;
      }
    }
  }

  response += "]}";
  file.close();
  
  Serial.printf("[DB] Найдено профилей: %d\n", profileCount);
  Serial.printf("[DB] Размер ответа: %d байт\n", response.length());
  
  // Если данных нет
  if (profileCount == 0) {
    Serial.println("[DB] ВНИМАНИЕ: Профили не найдены в файле!");
    pDbSyncChar->setValue(response.c_str());
    pDbSyncChar->notify();
    Serial.println("[DB] === Отправка завершена (пустой список) ===");
    sendingProfilesNow = false;
    return;
  }
  
  // Chunking: разбиваем на фрагменты
  // Flutter собирает data из всех чанков и парсит как единый JSON
  const int CHUNK_SIZE = 280;  // Уменьшено для запаса после экранирования
  int totalLength = response.length();
  int totalChunks = (totalLength + CHUNK_SIZE - 1) / CHUNK_SIZE;
  
  Serial.printf("[DB] Отправка с chunking: %d чанков по ~%d байт\n", totalChunks, CHUNK_SIZE);
  
  for (int i = 0; i < totalChunks; i++) {
    int start = i * CHUNK_SIZE;
    int end = min(start + CHUNK_SIZE, totalLength);
    String dataChunk = response.substring(start, end);
    
    // Экранируем для вложения в JSON строку
    String escapedData;
    escapedData.reserve(dataChunk.length() * 2);  // Резервируем память с запасом
    
    for (int j = 0; j < dataChunk.length(); j++) {
      char c = dataChunk[j];
      if (c == '\\') escapedData += "\\\\";
      else if (c == '\"') escapedData += "\\\"";
      else if (c == '\n') escapedData += "\\n";
      else if (c == '\r') escapedData += "\\r";
      else if (c == '\t') escapedData += "\\t";
      else escapedData += c;
    }
    
    // Формируем чанк в формате для Flutter
    String chunk = "{\"cmd\":\"database_chunk\",\"index\":" + String(i) + 
                   ",\"total\":" + String(totalChunks) + 
                   ",\"data\":\"" + escapedData + "\"}";
    
    // Проверка размера чанка
    if (chunk.length() > 500) {
      Serial.printf("[DB] WARNING: chunk %d too large: %d bytes!\n", i + 1, chunk.length());
    }
    
    Serial.printf("[DB] Отправка чанка %d/%d (%d байт)\n", i + 1, totalChunks, chunk.length());
    
    pDbSyncChar->setValue(chunk.c_str());
    pDbSyncChar->notify();
    
    // Задержка для стабильности BLE
    delay(20);
  }
  
  Serial.println("[DB] === Отправка завершена (chunking) ===");
  sendingProfilesNow = false;
}

void sendFullProfile(String filamentId) {
  if (!pDbSyncChar) {
    Serial.println("[DB] ERROR: pDbSyncChar == NULL");
    return;
  }
  
  File file = LittleFS.open("/filaments.json", "r");
  if (!file) {
    Serial.println("[DB] Не удалось открыть filaments.json");
    String response = "{\"cmd\":\"profile_data\",\"success\":false}";
    pDbSyncChar->setValue(response.c_str());
    pDbSyncChar->notify();
    return;
  }
  
  String line;
  bool found = false;
  
  while (file.available()) {
    line = file.readStringUntil('\n');
    if (line.indexOf(filamentId) >= 0) {
      found = true;
      break;
    }
  }
  file.close();
  
  if (found) {
    String response = "{\"cmd\":\"profile_data\",\"success\":true,\"data\":" + line + "}";
    Serial.printf("[DB] Отправка профиля %s (%d байт)\n", filamentId.c_str(), response.length());
    pDbSyncChar->setValue(response.c_str());
    pDbSyncChar->notify();
  } else {
    String response = "{\"cmd\":\"profile_data\",\"success\":false}";
    pDbSyncChar->setValue(response.c_str());
    pDbSyncChar->notify();
    Serial.printf("[DB] Профиль %s не найден\n", filamentId.c_str());
  }
}

// Добавление нового профиля в базу данных
bool addProfileToDatabase(String profileJson) {
  // Извлекаем ID из JSON
  String id = "";
  int idx = profileJson.indexOf("\"id\":\"");
  if (idx >= 0) {
    idx += 6;
    id = profileJson.substring(idx, profileJson.indexOf("\"", idx));
  }
  
  if (id.length() == 0) {
    Serial.println("[DB] Ошибка: ID не найден в профиле");
    return false;
  }
  
  // Проверяем, существует ли уже профиль с таким ID
  File file = LittleFS.open("/filaments.json", "r");
  if (file) {
    while (file.available()) {
      String line = file.readStringUntil('\n');
      if (line.indexOf("\"id\":\"" + id + "\"") >= 0) {
        file.close();
        Serial.printf("[DB] Профиль %s уже существует\n", id.c_str());
        return false; // Профиль уже есть
      }
    }
    file.close();
  }
  
  // Добавляем профиль в конец файла
  file = LittleFS.open("/filaments.json", "a");
  if (!file) {
    Serial.println("[DB] Не удалось открыть filaments.json для записи");
    return false;
  }
  
  file.println(profileJson);
  file.close();
  
  Serial.printf("[DB] Профиль %s добавлен в базу\n", id.c_str());
  return true;
}
