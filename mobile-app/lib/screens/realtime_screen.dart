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
          
          // Получаем профиль филамента
          final filamentId = data?.currentFilamentId;
          final currentProfile = filamentId != null 
            ? bt.profiles.where((p) => p.id == filamentId).firstOrNull
            : null;
          
          final total = currentProfile?.weight ?? 1000.0;
          final remaining = data?.netWeight ?? 0;
          final percent = total > 0 ? (remaining / total).clamp(0.0, 1.0) : 0.0;
          
          // Расчет длины филамента
          // Формула: объем = вес / плотность, длина = объем / площадь_сечения
          final density = currentProfile?.density ?? 1.24;  // г/см³
          final diameter = currentProfile?.diameter ?? 1.75;  // мм
          final radiusMm = diameter / 2;
          final areaMm2 = 3.14159 * radiusMm * radiusMm;  // мм²
          final areaCm2 = areaMm2 / 100;  // см² (1 см² = 100 мм²)
          final volumeCm3 = remaining / density;  // см³
          final lengthCm = volumeCm3 / areaCm2;  // см
          final lengthMeters = lengthCm / 100;  // метры
          
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
                if (currentProfile != null)
                  Card(
                    child: Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 16),
                      child: Column(
                        children: [
                          _CompactInfoRow('Производитель', currentProfile.manufacturer),
                          const SizedBox(height: 8),
                          _CompactInfoRow('Материал', currentProfile.material),
                          const SizedBox(height: 8),
                          _CompactInfoRow('Диаметр', '${currentProfile.diameter} мм'),
                          const SizedBox(height: 8),
                          _CompactInfoRow('Вес', '${remaining.toStringAsFixed(0)} г'),
                          const SizedBox(height: 8),
                          _CompactInfoRow('Длина', '${lengthMeters.toStringAsFixed(1)} м'),
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
