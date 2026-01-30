# Smart Filament Holder

IoT система мониторинга филамента для 3D-печати с автоматическим расчётом остатка и проверкой достаточности материала перед печатью.

## Архитектура

Система состоит из трёх компонентов:

- **Firmware** (ESP32-C6) - измерение веса, управление дисплеем, BLE/WiFi API
- **Mobile App** (Flutter/Android) - управление профилями, запись NFC меток
- **Desktop Plugin** (Python/PyQt6) - интеграция с Creality Print, автоматическая проверка

## Аппаратная часть

- ESP32-C6 (WiFi 6, BLE 5.0, 160MHz)
- GC9A01 1.28" 240x240 SPI дисплей + CST816S I2C тачскрин
- RC522 NFC reader (поддержка MIFARE Classic 1K и NTAG21x)
- HX711 24-bit ADC + тензодатчик 5kg
- База данных: 460 профилей филаментов (LittleFS)

## Функциональность

- Измерение веса с точностью ±1г
- Расчёт остатка филамента (длина, процент)
- Загрузка профилей через NFC/BLE
- REST API и BLE GATT сервер
- Автоматическая проверка перед печатью (desktop plugin)

---

## Firmware

### Требования

- Arduino IDE 2.x
- ESP32 board support 3.0+
- Библиотеки: HX711, MFRC522, ArduinoJson, LVGL 9.4+, Arduino_GFX

### Настройка платы

```
Board: ESP32C6 Dev Module
Flash Size: 8MB
Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)
Upload Speed: 921600
```

### Установка

1. Скопировать `config.h.example` → `config.h`
2. Настроить WiFi credentials и пины
3. Установить ESP32FS plugin: https://github.com/lorol/arduino-esp32fs-plugin
4. Загрузить `data/filaments.json` через Tools → ESP32 Sketch Data Upload → LittleFS
5. Прошить через Upload

### Калибровка HX711

```bash
# Загрузить firmware/weight_calibrator/weight_calibrator.ino
# Следовать инструкциям в Serial Monitor (115200 baud)
# Скопировать CALIBRATION_FACTOR в config.h
```

### API

**BLE GATT** (Service UUID: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`)

- `beb5483e-36e1-4688-b7f5-ea07361b26a8` (Data) - notify, вес и статус
- `beb5483e-36e1-4688-b7f5-ea07361b26a9` (Command) - write, команды управления
- `beb5483e-36e1-4688-b7f5-ea07361b26aa` (DB Sync) - notify, синхронизация профилей

**HTTP REST API**

```
GET  /data    - JSON с весом и статусом
GET  /status  - статус устройства
POST /tare    - тарировка весов
```

**NFC Protocol**

Поддерживаемые NFC метки:
- **MIFARE Classic 1K/4K** - блоки по 16 байт, требует аутентификации
- **NTAG213/215/216** - страницы по 4 байта, без аутентификации

Профиль ID записывается в NDEF Text Record формате (до 40 символов).
Рекомендуются **NTAG216** (888 байт памяти) - дешевле и проще в использовании.

### Debug режим

При `DEBUG_MODE = true` доступны Serial команды:

```
w<value>  - установить вес (w850)
t         - tare
p<value>  - процент (p75.5)
l<value>  - длина (l250)
id <id>   - загрузить профиль (id 3d-fuel_pla+_1000.0_1.75)
1-6       - переключить экран
```

---

## Mobile App

Flutter приложение для Android с BLE и NFC поддержкой.

### Сборка

```bash
cd mobile-app
flutter pub get
flutter build apk --release
```

Результат: `build/app/outputs/flutter-apk/app-release.apk` (47.4 MB)

### Зависимости

- flutter_blue_plus (BLE)
- nfc_manager (NFC)
- provider (state management)
- permission_handler

### Структура

```
lib/
├── models/       - Filament data model
├── screens/      - UI (home, realtime, filament_list)
├── services/     - BLE, NFC, database
└── utils/        - permissions
```

---

## Desktop Plugin

Windows приложение для интеграции с Creality Print.

### Сборка

```bash
cd desktop-plugin
pip install -r requirements.txt
pyinstaller --onefile --noconsole --icon=filament_checker.ico --name FilamentChecker main.py
```

Результат: `dist/FilamentChecker.exe` (39.8 MB)

### Принцип работы

1. Мониторинг запуска Creality Print (psutil)
2. Сканирование сети 192.168.x.x:80,81 для поиска устройств FD-*
3. Парсинг G-code из временных папок
4. Расчёт требуемого веса филамента
5. Сравнение с доступным весом через HTTP API
6. Уведомление пользователя

### Конфигурация

```python
GCODE_TEMP_FOLDERS = [...]  # Пути к временным G-code
ESP32_PORTS = [80, 81]      # Порты для HTTP API
HOLDER_PREFIX = "FD"        # Префикс имени устройства
```

---

## Releases

Готовые сборки в `releases/`:

- `FilamentWeight-v1.0.0.apk` (47.4 MB)
- `FilamentChecker-v1.0.0.exe` (39.8 MB)

---

## Troubleshooting

**HX711 не отвечает**: установить `DEBUG_MODE = true` для тестирования без датчика

**Профили не загружаются**: проверить загрузку `filaments.json` через LittleFS Data Upload

**Ошибка компиляции**: использовать Partition Scheme "Huge APP (3MB No OTA/1MB SPIFFS)"

**BLE не подключается**: проверить MTU (должен быть 512 байт)

**NFC метка не читается**: проверить тип метки (поддерживаются MIFARE Classic и NTAG21x). Для NTAG убедитесь что метка не защищена паролем.
