import 'dart:io';
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:device_info_plus/device_info_plus.dart';

class PermissionsHelper {
  /// Запросить все необходимые разрешения для BLE
  static Future<bool> requestBluetoothPermissions() async {
    if (!Platform.isAndroid) {
      return true; // iOS не требует этих разрешений
    }

    try {
      // Для Android 12+ (API 31+)
      if (Platform.isAndroid) {
        final androidInfo = await _getAndroidVersion();
        
        if (androidInfo >= 31) {
          // Android 12+
          debugPrint('[Permissions] Requesting Android 12+ permissions...');
          
          Map<Permission, PermissionStatus> statuses = await [
            Permission.bluetoothScan,
            Permission.bluetoothConnect,
          ].request();
          
          bool allGranted = statuses.values.every((status) => status.isGranted);
          
          if (!allGranted) {
            debugPrint('[Permissions] Some permissions denied:');
            statuses.forEach((permission, status) {
              debugPrint('  $permission: $status');
            });
          }
          
          return allGranted;
        } else {
          // Android 6-11
          debugPrint('[Permissions] Requesting Android 6-11 permissions...');
          
          Map<Permission, PermissionStatus> statuses = await [
            Permission.bluetooth,
            Permission.location,
          ].request();
          
          bool allGranted = statuses.values.every((status) => status.isGranted);
          
          if (!allGranted) {
            debugPrint('[Permissions] Some permissions denied:');
            statuses.forEach((permission, status) {
              debugPrint('  $permission: $status');
            });
          }
          
          return allGranted;
        }
      }
    } catch (e) {
      debugPrint('[Permissions] Error requesting permissions: $e');
      return false;
    }
    
    return true;
  }

  /// Проверить статус разрешений BLE
  static Future<bool> checkBluetoothPermissions() async {
    if (!Platform.isAndroid) {
      return true;
    }

    try {
      final androidInfo = await _getAndroidVersion();
      
      if (androidInfo >= 31) {
        // Android 12+
        final scanStatus = await Permission.bluetoothScan.status;
        final connectStatus = await Permission.bluetoothConnect.status;
        
        return scanStatus.isGranted && connectStatus.isGranted;
      } else {
        // Android 6-11
        final bluetoothStatus = await Permission.bluetooth.status;
        final locationStatus = await Permission.location.status;
        
        return bluetoothStatus.isGranted && locationStatus.isGranted;
      }
    } catch (e) {
      debugPrint('[Permissions] Error checking permissions: $e');
      return false;
    }
  }

  /// Открыть настройки приложения
  static Future<void> openAppSettings() async {
    await openAppSettings();
  }

  /// Получить версию Android
  static Future<int> _getAndroidVersion() async {
    try {
      final deviceInfo = DeviceInfoPlugin();
      final androidInfo = await deviceInfo.androidInfo;
      return androidInfo.version.sdkInt;
    } catch (e) {
      debugPrint('[Permissions] Error getting Android version: $e');
      // По умолчанию считаем что Android 12+
      return 31;
    }
  }

  /// Показать диалог с объяснением зачем нужны разрешения
  static Future<bool> showPermissionRationale(BuildContext context) async {
    return await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Разрешения Bluetooth'),
        content: const Text(
          'Для поиска и подключения к устройству FD-01 необходимы разрешения:\n\n'
          '• Bluetooth - для подключения к устройству\n'
          '• Местоположение - требуется Android для сканирования BLE устройств\n\n'
          'Ваше местоположение НЕ отслеживается.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context, false),
            child: const Text('Отмена'),
          ),
          ElevatedButton(
            onPressed: () => Navigator.pop(context, true),
            child: const Text('Предоставить'),
          ),
        ],
      ),
    ) ?? false;
  }
}
