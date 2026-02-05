/*
 * КАЛИБРОВЩИК ВЕСОВ HX711
*/
#include <HX711.h>

// ПИНЫ HX711 (должны совпадать с config.h)
#define HX711_DOUT 25
#define HX711_SCK  26

HX711 scale;

// SETUP
void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n╔════════════════════════════════════╗");
  Serial.println("║  КАЛИБРОВЩИК ВЕСОВ HX711          ║");
  Serial.println("╚════════════════════════════════════╝\n");
  
  Serial.println("[1/3] Инициализация HX711...");
  scale.begin(HX711_DOUT, HX711_SCK);
  
  if (!scale.wait_ready_timeout(1000)) {
    Serial.println("✗ HX711 не отвечает!");
    Serial.println("Проверь подключение:");
    Serial.printf("  DOUT -> GPIO%d\n", HX711_DOUT);
    Serial.printf("  SCK  -> GPIO%d\n", HX711_SCK);
    Serial.println("  VCC  -> 3.3V или 5V");
    Serial.println("  GND  -> GND");
    while(1) delay(1000);
  }
  
  Serial.println("✓ HX711 подключен");
  Serial.printf("Текущее значение: %ld\n\n", scale.read());
  
  // ШАГ 1: Тарировка (обнуление)
  Serial.println("[2/3] ТАРИРОВКА");
  Serial.println("Убери ВСЁ с весов и нажми Enter...");
  waitForEnter();
  
  Serial.println("Обнуление...");
  scale.tare(20);  // Усредняем 20 измерений
  Serial.println("✓ Весы обнулены\n");
  
  // ШАГ 2: Калибровка с известным весом
  Serial.println("[3/3] КАЛИБРОВКА");
  Serial.println("Положи на весы известный вес (например, 100г, 500г, 1000г)");
  Serial.println("Введи вес в граммах и нажми Enter:");
  
  float knownWeight = readFloat();
  Serial.printf("Известный вес: %.1f г\n", knownWeight);
  Serial.println("Измеряю...");
  
  delay(1000);
  long reading = scale.get_units(20);  // Усредняем 20 измерений
  
  Serial.printf("Сырое значение: %ld\n", reading);
  
  if (reading == 0) {
    Serial.println("✗ Ошибка: нулевое значение!");
    Serial.println("Проверь что груз действительно на весах");
    while(1) delay(1000);
  }
  
  // Вычисляем калибровочный коэффициент
  float calibrationFactor = (float)reading / knownWeight;
  
  Serial.println("\n╔════════════════════════════════════╗");
  Serial.println("║  КАЛИБРОВКА ЗАВЕРШЕНА!            ║");
  Serial.println("╚════════════════════════════════════╝\n");
  
  Serial.printf("CALIBRATION_FACTOR = %.2f\n\n", calibrationFactor);
  
  Serial.println("Скопируй это значение в config.h:");
  Serial.println("-----------------------------------");
  Serial.printf("#define CALIBRATION_FACTOR %.2ff\n", calibrationFactor);
  Serial.println("-----------------------------------\n");
  
  // Применяем калибровку и тестируем
  scale.set_scale(calibrationFactor);
  
  Serial.println("ТЕСТ: Проверка точности");
  Serial.println("Положи разные грузы на весы для проверки\n");
}

// LOOP - Постоянное измерение
void loop() {
  if (scale.wait_ready_timeout(200)) {
    float weight = scale.get_units(5);
    
    Serial.print("Вес: ");
    Serial.print(weight, 1);
    Serial.println(" г");
    
    delay(500);
  }
}

// ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
void waitForEnter() {
  while (!Serial.available()) {
    delay(100);
  }
  while (Serial.available()) {
    Serial.read();  // Очищаем буфер
  }
}

float readFloat() {
  while (!Serial.available()) {
    delay(100);
  }
  
  String input = Serial.readStringUntil('\n');
  input.trim();
  return input.toFloat();
}
