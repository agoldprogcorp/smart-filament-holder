import 'dart:async';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'package:intl/intl.dart';
import '../services/bluetooth_service.dart';

class RealtimeScreen extends StatefulWidget {
  const RealtimeScreen({super.key});

  @override
  State<RealtimeScreen> createState() => _RealtimeScreenState();
}

class _RealtimeScreenState extends State<RealtimeScreen> {
  late Timer _clockTimer;
  DateTime _currentTime = DateTime.now();

  @override
  void initState() {
    super.initState();
    // Обновляем время каждую секунду
    _clockTimer = Timer.periodic(const Duration(seconds: 1), (timer) {
      setState(() {
        _currentTime = DateTime.now();
      });
    });
  }

  @override
  void dispose() {
    _clockTimer.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    // Время по МСК (UTC+3)
    final moscowTime = _currentTime.toUtc().add(const Duration(hours: 3));
    final timeString = DateFormat('HH:mm:ss').format(moscowTime);
    final dateString = DateFormat('dd.MM.yyyy').format(moscowTime);

    return Scaffold(
      appBar: AppBar(
        title: const Text('Мониторинг'),
      ),
      body: Consumer<BluetoothService>(
        builder: (context, bt, _) {
          if (!bt.isConnected) {
            return Center(
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  const Icon(Icons.bluetooth_disabled, size: 64, color: Colors.grey),
                  const SizedBox(height: 16),
                  const Text('Устройство не подключено'),
                  const SizedBox(height: 8),
                  const Text(
                    'Подключитесь к держателю\nна главном экране',
                    textAlign: TextAlign.center,
                    style: TextStyle(color: Colors.grey),
                  ),
                  const Spacer(),
                  _buildTimeDisplay(timeString, dateString),
                ],
              ),
            );
          }

          final data = bt.holderData;

          // Используем данные напрямую от ESP32
          final grossWeight = data?.grossWeight ?? 0;
          final netWeight = data?.netWeight ?? 0;
          final percentValue = data?.percent ?? 0;
          final percent = (percentValue / 100).clamp(0.0, 1.0);

          // Данные филамента - обрабатываем пустые строки
          final rawMaterial = data?.material ?? '';
          final rawManufacturer = data?.manufacturer ?? '';
          final material = (rawMaterial.isNotEmpty && rawMaterial != '?') ? rawMaterial : '—';
          final manufacturer = (rawManufacturer.isNotEmpty && rawManufacturer != '?') ? rawManufacturer : '—';
          final diameter = data?.diameter ?? 1.75;
          final lengthMeters = (data?.length ?? 0).toDouble();
          final profileLoaded = data?.profileLoaded ?? false;

          return SingleChildScrollView(
            padding: const EdgeInsets.all(16),
            child: Column(
              children: [
                // Круговой прогресс-бар с процентом
                Card(
                  child: Padding(
                    padding: const EdgeInsets.all(20),
                    child: Column(
                      children: [
                        SizedBox(
                          width: 160,
                          height: 160,
                          child: Stack(
                            alignment: Alignment.center,
                            children: [
                              SizedBox(
                                width: 160,
                                height: 160,
                                child: CircularProgressIndicator(
                                  value: percent,
                                  strokeWidth: 14,
                                  backgroundColor: Colors.grey.shade300,
                                  valueColor: AlwaysStoppedAnimation(
                                    percent > 0.2 ? Colors.green : Colors.orange,
                                  ),
                                ),
                              ),
                              Text(
                                '${(percent * 100).toStringAsFixed(0)}%',
                                style: TextStyle(
                                  fontSize: 36,
                                  fontWeight: FontWeight.bold,
                                  color: percent > 0.2 ? Colors.green : Colors.orange,
                                ),
                              ),
                            ],
                          ),
                        ),
                        const SizedBox(height: 8),
                        Text(
                          profileLoaded ? 'Филамент загружен' : 'Профиль не загружен',
                          style: TextStyle(
                            fontSize: 14,
                            color: profileLoaded ? Colors.green : Colors.grey,
                          ),
                        ),
                      ],
                    ),
                  ),
                ),
                const SizedBox(height: 12),

                // Информация о весе
                Card(
                  child: Padding(
                    padding: const EdgeInsets.all(16),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const Text(
                          'Вес',
                          style: TextStyle(
                            fontSize: 16,
                            fontWeight: FontWeight.bold,
                          ),
                        ),
                        const SizedBox(height: 12),
                        _InfoRow('Общий вес', '${grossWeight.toStringAsFixed(0)} г'),
                        const SizedBox(height: 8),
                        _InfoRow('Вес филамента', '${netWeight.toStringAsFixed(0)} г'),
                        const SizedBox(height: 8),
                        _InfoRow('Длина', lengthMeters > 0 ? '${lengthMeters.toStringAsFixed(0)} м' : '— м'),
                      ],
                    ),
                  ),
                ),
                const SizedBox(height: 12),

                // Информация о филаменте
                Card(
                  child: Padding(
                    padding: const EdgeInsets.all(16),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const Text(
                          'Филамент',
                          style: TextStyle(
                            fontSize: 16,
                            fontWeight: FontWeight.bold,
                          ),
                        ),
                        const SizedBox(height: 12),
                        _InfoRow('Производитель', manufacturer),
                        const SizedBox(height: 8),
                        _InfoRow('Материал', material),
                        const SizedBox(height: 8),
                        _InfoRow('Диаметр', '${diameter.toStringAsFixed(2)} мм'),
                      ],
                    ),
                  ),
                ),
                const SizedBox(height: 16),

                // Время
                _buildTimeDisplay(timeString, dateString),
                const SizedBox(height: 16),
              ],
            ),
          );
        },
      ),
    );
  }

  Widget _buildTimeDisplay(String time, String date) {
    return Card(
      color: Colors.grey.shade100,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 12),
        child: Column(
          children: [
            Text(
              time,
              style: const TextStyle(
                fontSize: 32,
                fontWeight: FontWeight.bold,
                fontFamily: 'monospace',
              ),
            ),
            Text(
              '$date (МСК)',
              style: TextStyle(
                fontSize: 14,
                color: Colors.grey.shade600,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _InfoRow extends StatelessWidget {
  final String label;
  final String value;

  const _InfoRow(this.label, this.value);

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisAlignment: MainAxisAlignment.spaceBetween,
      children: [
        Text(
          label,
          style: TextStyle(
            fontSize: 14,
            color: Colors.grey.shade600,
          ),
        ),
        Text(
          value,
          style: const TextStyle(
            fontSize: 14,
            fontWeight: FontWeight.w600,
          ),
        ),
      ],
    );
  }
}
