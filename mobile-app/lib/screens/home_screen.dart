import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../services/bluetooth_service.dart';
import '../utils/permissions.dart';
import 'filament_list_screen.dart';
import 'realtime_screen.dart';

class HomeScreen extends StatelessWidget {
  const HomeScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Consumer<BluetoothService>(
      builder: (context, bt, _) {
        return Scaffold(
          appBar: AppBar(
            title: const Text('Filament Weight'),
            actions: [
              IconButton(
                icon: Icon(bt.isConnected ? Icons.bluetooth_connected : Icons.bluetooth),
                onPressed: () => _showBluetoothDialog(context, bt),
              ),
            ],
          ),
          body: Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                _buildConnectionCard(context, bt),
                const SizedBox(height: 16),
                Expanded(
                  child: GridView.count(
                    crossAxisCount: 2,
                    mainAxisSpacing: 16,
                    crossAxisSpacing: 16,
                    children: [
                      _buildMenuCard(
                        context,
                        icon: Icons.list,
                        title: 'Филаменты',
                        subtitle: 'Выбор профиля',
                        onTap: () => Navigator.push(
                          context,
                          MaterialPageRoute(builder: (_) => const FilamentListScreen()),
                        ),
                      ),
                      _buildMenuCard(
                        context,
                        icon: Icons.monitor_weight,
                        title: 'Мониторинг',
                        subtitle: 'Данные в реальном времени',
                        onTap: () => Navigator.push(
                          context,
                          MaterialPageRoute(builder: (_) => const RealtimeScreen()),
                        ),
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),
        );
      },
    );
  }

  Widget _buildConnectionCard(BuildContext context, BluetoothService bt) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Row(
          children: [
            Icon(
              bt.isConnected ? Icons.check_circle : Icons.error_outline,
              color: bt.isConnected ? Colors.green : Colors.orange,
              size: 40,
            ),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    bt.isConnected ? 'Подключено' : 'Не подключено',
                    style: Theme.of(context).textTheme.titleMedium,
                  ),
                  Text(
                    bt.isConnected 
                      ? bt.connectedDeviceName ?? 'ESP32' 
                      : 'Нажмите для подключения',
                    style: Theme.of(context).textTheme.bodySmall,
                  ),
                ],
              ),
            ),
            if (bt.isConnected)
              TextButton(
                onPressed: bt.disconnect,
                child: const Text('Отключить'),
              ),
          ],
        ),
      ),
    );
  }

  Widget _buildMenuCard(
    BuildContext context, {
    required IconData icon,
    required String title,
    required String subtitle,
    required VoidCallback onTap,
  }) {
    return Card(
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(12),
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Icon(icon, size: 48, color: Theme.of(context).primaryColor),
              const SizedBox(height: 8),
              Text(title, style: Theme.of(context).textTheme.titleMedium),
              Text(subtitle, style: Theme.of(context).textTheme.bodySmall),
            ],
          ),
        ),
      ),
    );
  }

  void _showBluetoothDialog(BuildContext context, BluetoothService bt) {
    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Bluetooth устройства'),
        content: SizedBox(
          width: 300,
          height: 400,
          child: StatefulBuilder(
            builder: (context, setState) {
              return Column(
                children: [
                  ElevatedButton.icon(
                    onPressed: bt.isScanning ? null : () async {
                      // Запрашиваем разрешения перед сканированием
                      final hasPermissions = await PermissionsHelper.requestBluetoothPermissions();
                      
                      if (!hasPermissions) {
                        if (context.mounted) {
                          ScaffoldMessenger.of(context).showSnackBar(
                            const SnackBar(
                              content: Text('Необходимы разрешения Bluetooth для сканирования'),
                              duration: Duration(seconds: 3),
                            ),
                          );
                        }
                        return;
                      }
                      
                      bt.startScan();
                      setState(() {});
                    },
                    icon: bt.isScanning 
                      ? const SizedBox(
                          width: 16, height: 16,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        )
                      : const Icon(Icons.search),
                    label: Text(bt.isScanning ? 'Поиск...' : 'Сканировать'),
                  ),
                  const SizedBox(height: 8),
                  OutlinedButton.icon(
                    onPressed: () {
                      bt.startSimulation();
                      Navigator.pop(context);
                    },
                    icon: const Icon(Icons.bug_report),
                    label: const Text('Симуляция'),
                  ),
                  const SizedBox(height: 16),
                  Expanded(
                    child: Consumer<BluetoothService>(
                      builder: (context, bt, _) {
                        if (bt.scanResults.isEmpty) {
                          return const Center(
                            child: Text('Устройства не найдены\nНажмите "Сканировать"'),
                          );
                        }
                        return ListView.builder(
                          itemCount: bt.scanResults.length,
                          itemBuilder: (context, index) {
                            final result = bt.scanResults[index];
                            return ListTile(
                              leading: const Icon(Icons.bluetooth),
                              title: Text(result.device.platformName.isNotEmpty 
                                ? result.device.platformName 
                                : 'Unknown'),
                              subtitle: Text('RSSI: ${result.rssi}'),
                              onTap: () async {
                                Navigator.pop(context);
                                final success = await bt.connect(result.device);
                                if (!success && context.mounted) {
                                  ScaffoldMessenger.of(context).showSnackBar(
                                    const SnackBar(content: Text('Ошибка подключения')),
                                  );
                                }
                              },
                            );
                          },
                        );
                      },
                    ),
                  ),
                ],
              );
            },
          ),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: const Text('Закрыть'),
          ),
        ],
      ),
    );
  }
}
