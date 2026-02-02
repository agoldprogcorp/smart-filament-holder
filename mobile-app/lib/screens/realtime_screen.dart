import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../services/bluetooth_service.dart';

class RealtimeScreen extends StatefulWidget {
  const RealtimeScreen({super.key});

  @override
  State<RealtimeScreen> createState() => _RealtimeScreenState();
}

class _RealtimeScreenState extends State<RealtimeScreen> {
  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Мониторинг'),
      ),
      body: Consumer<BluetoothService>(
        builder: (context, bt, _) {
          if (!bt.isConnected) {
            return const Center(
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Icon(Icons.bluetooth_disabled, size: 64, color: Colors.grey),
                  SizedBox(height: 16),
                  Text('Устройство не подключено'),
                  SizedBox(height: 8),
                  Text(
                    'Подключитесь к держателю\nна главном экране',
                    textAlign: TextAlign.center,
                    style: TextStyle(color: Colors.grey),
                  ),
                ],
              ),
            );
          }

          final data = bt.holderData;

          // Используем данные напрямую от ESP32
          final remaining = data?.netWeight ?? 0;
          final percentValue = data?.percent ?? 0;  // ESP32 уже отправляет процент
          final percent = (percentValue / 100).clamp(0.0, 1.0);  // Конвертируем в 0-1

          // Данные филамента напрямую от ESP32
          // Обрабатываем пустые строки как "?"
          final rawMaterial = data?.material ?? '';
          final rawManufacturer = data?.manufacturer ?? '';
          final material = rawMaterial.isNotEmpty && rawMaterial != '?' ? rawMaterial : '?';
          final manufacturer = rawManufacturer.isNotEmpty && rawManufacturer != '?' ? rawManufacturer : '?';
          final diameter = data?.diameter ?? 1.75;
          final lengthMeters = (data?.length ?? 0).toDouble();  // ESP32 уже отправляет длину в метрах
          final profileLoaded = data?.profileLoaded ?? false;
          
          return SingleChildScrollView(
            padding: const EdgeInsets.all(16),
            child: Column(
              children: [
                const SizedBox(height: 40), // Отступ сверху
                // Круговой прогресс-бар с процентом
                Card(
                  child: Padding(
                    padding: const EdgeInsets.all(24),
                    child: Center(
                      child: SizedBox(
                        width: 200,
                        height: 200,
                        child: Stack(
                          alignment: Alignment.center,
                          children: [
                            // Круговой прогресс
                            SizedBox(
                              width: 200,
                              height: 200,
                              child: CircularProgressIndicator(
                                value: percent,
                                strokeWidth: 16,
                                backgroundColor: Colors.grey.shade300,
                                valueColor: AlwaysStoppedAnimation(
                                  percent > 0.2 ? Colors.green : Colors.orange,
                                ),
                              ),
                            ),
                            // Процент в центре
                            Column(
                              mainAxisAlignment: MainAxisAlignment.center,
                              children: [
                                Text(
                                  '${(percent * 100).toStringAsFixed(0)}%',
                                  style: Theme.of(context).textTheme.displayLarge?.copyWith(
                                    fontWeight: FontWeight.bold,
                                    color: percent > 0.2 ? Colors.green : Colors.orange,
                                  ),
                                ),
                                const SizedBox(height: 4),
                                Text(
                                  'осталось',
                                  style: TextStyle(
                                    fontSize: 14,
                                    color: Colors.grey.shade600,
                                  ),
                                ),
                              ],
                            ),
                          ],
                        ),
                      ),
                    ),
                  ),
                ),
                const SizedBox(height: 20),
                // Информация о филаменте (компактная)
                if (profileLoaded)
                  Card(
                    child: Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 16),
                      child: Column(
                        children: [
                          _CompactInfoRow('Производитель', manufacturer),
                          const SizedBox(height: 8),
                          _CompactInfoRow('Материал', material),
                          const SizedBox(height: 8),
                          _CompactInfoRow('Диаметр', '${diameter.toStringAsFixed(2)} мм'),
                          const SizedBox(height: 8),
                          _CompactInfoRow('Вес', '${remaining.toStringAsFixed(0)} г'),
                          const SizedBox(height: 8),
                          _CompactInfoRow('Длина', '${lengthMeters.toStringAsFixed(0)} м'),
                        ],
                      ),
                    ),
                  ),
                const SizedBox(height: 16),
              ],
            ),
          );
        },
      ),
    );
  }
}

class _CompactInfoRow extends StatelessWidget {
  final String label;
  final String value;

  const _CompactInfoRow(this.label, this.value);

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Text(
          '$label:',
          style: const TextStyle(fontSize: 15, color: Colors.grey),
        ),
        const SizedBox(width: 8),
        Expanded(
          child: Text(
            value,
            style: const TextStyle(fontSize: 15, fontWeight: FontWeight.bold),
          ),
        ),
      ],
    );
  }
}
