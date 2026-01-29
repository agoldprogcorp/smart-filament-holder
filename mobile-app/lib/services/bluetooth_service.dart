import 'dart:async';
import 'dart:convert';
import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart' as ble;
import '../models/filament.dart';

class HolderData {
  final double grossWeight;
  final double netWeight;
  final double spoolWeight;
  final String? currentFilamentId;
  final String? material;
  final String? manufacturer;
  final double? percent;
  final int? length;
  final double? diameter;
  final double? density;
  final int? bedTemp;
  final String? status;
  final bool? profileLoaded;

  HolderData({
    required this.grossWeight,
    required this.netWeight,
    required this.spoolWeight,
    this.currentFilamentId,
    this.material,
    this.manufacturer,
    this.percent,
    this.length,
    this.diameter,
    this.density,
    this.bedTemp,
    this.status,
    this.profileLoaded,
  });

  factory HolderData.fromJson(Map<String, dynamic> json) {
    return HolderData(
      grossWeight: (json['gross'] ?? 0.0).toDouble(),
      netWeight: (json['net'] ?? 0.0).toDouble(),
      spoolWeight: (json['spool'] ?? 0.0).toDouble(),
      currentFilamentId: json['filament_id'],
      material: json['material'],
      manufacturer: json['manufacturer'],
      percent: json['percent']?.toDouble(),
      length: json['length'],
      diameter: json['diameter']?.toDouble(),
      density: json['density']?.toDouble(),
      bedTemp: json['bed_temp'],
      status: json['status'],
      profileLoaded: json['profile_loaded'],
    );
  }
}

class BluetoothService extends ChangeNotifier {
  static const String serviceUuid = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
  static const String dataCharUuid = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
  static const String cmdCharUuid = "beb5483e-36e1-4688-b7f5-ea07361b26a9";
  static const String dbSyncCharUuid = "beb5483e-36e1-4688-b7f5-ea07361b26aa";

  ble.BluetoothDevice? _connectedDevice;
  ble.BluetoothCharacteristic? _dataChar;
  ble.BluetoothCharacteristic? _cmdChar;
  ble.BluetoothCharacteristic? _dbSyncChar;
  StreamSubscription? _dataSubscription;
  StreamSubscription? _dbSyncSubscription;
  StreamSubscription? _scanSubscription;
  StreamSubscription? _connectionStateSubscription;
  Timer? _simulationTimer;
  Completer<Filament?>? _profileCompleter;
  
  bool _isScanning = false;
  bool _isConnected = false;
  bool _isSimulation = false;
  List<ble.ScanResult> _scanResults = [];
  HolderData? _holderData;
  List<Filament> _profiles = [];
  final List<String> _dbChunks = [];
  bool _isLoadingProfiles = false;

  bool get isScanning => _isScanning;
  bool get isConnected => _isConnected;
  bool get isSimulation => _isSimulation;
  List<ble.ScanResult> get scanResults => _scanResults;
  HolderData? get holderData => _holderData;
  String? get connectedDeviceName => _isSimulation ? 'Симуляция' : _connectedDevice?.platformName;
  List<Filament> get profiles => _profiles;
  bool get isLoadingProfiles => _isLoadingProfiles;

  Future<void> startScan() async {
    if (_isScanning) return;
    
    _scanResults.clear();
    _isScanning = true;
    notifyListeners();

    try {
      // Check if Bluetooth is available
      if (await ble.FlutterBluePlus.isSupported == false) {
        debugPrint('[BLE] Bluetooth not supported');
        _isScanning = false;
        notifyListeners();
        return;
      }

      // Check if Bluetooth is on
      var adapterState = await ble.FlutterBluePlus.adapterState.first;
      if (adapterState != ble.BluetoothAdapterState.on) {
        debugPrint('[BLE] Bluetooth is off');
        _isScanning = false;
        notifyListeners();
        return;
      }

      debugPrint('[BLE] Starting scan with Service UUID: $serviceUuid');

      await ble.FlutterBluePlus.startScan(
        timeout: const Duration(seconds: 10),
        androidUsesFineLocation: true,
      );
      
      _scanSubscription = ble.FlutterBluePlus.scanResults.listen((results) {
        debugPrint('[BLE] Scan results: ${results.length} devices found');
        
        _scanResults = results.where((r) {
          final name = r.device.platformName.toLowerCase();
          
          // Проверяем наличие нашего Service UUID в advertising data
          final hasOurService = r.advertisementData.serviceUuids.any(
            (u) => u.toString().toLowerCase().replaceAll('-', '') == 
                   serviceUuid.toLowerCase().replaceAll('-', '')
          );
          
          // Показываем устройство если:
          // 1. Есть наш Service UUID в advertising
          // 2. Или имя начинается с "fd" (FD-01)
          // 3. Или имя содержит "filament"
          final match = hasOurService || 
                        name.startsWith('fd') || 
                        name.contains('filament');
          
          if (match) {
            debugPrint('[BLE] Found matching device: ${r.device.platformName} (${r.device.remoteId})');
            debugPrint('      Services: ${r.advertisementData.serviceUuids}');
          }
          
          return match;
        }).toList();
        
        debugPrint('[BLE] Filtered results: ${_scanResults.length} devices');
        
        notifyListeners();
      });
    } catch (e) {
      debugPrint('[BLE] Scan error: $e');
      _isScanning = false;
      notifyListeners();
    }

    await Future.delayed(const Duration(seconds: 10));
    await stopScan();
  }

  Future<void> stopScan() async {
    await _scanSubscription?.cancel();
    _scanSubscription = null;
    await ble.FlutterBluePlus.stopScan();
    _isScanning = false;
    notifyListeners();
  }


  Future<bool> connect(ble.BluetoothDevice device) async {
    try {
      debugPrint('[BLE] ===== CONNECTING =====');
      debugPrint('[BLE] Device: ${device.platformName} (${device.remoteId})');
      
      await device.connect(timeout: const Duration(seconds: 15));
      _connectedDevice = device;
      debugPrint('[BLE] Connected successfully');

      try {
        final mtu = await device.requestMtu(512);
        debugPrint('[BLE] MTU set to: $mtu bytes');
      } catch (e) {
        debugPrint('[BLE] MTU request failed (not critical): $e');
      }

      debugPrint('[BLE] Discovering services...');
      List<ble.BluetoothService> services = await device.discoverServices();
      debugPrint('[BLE] Found ${services.length} services');
      
      for (var service in services) {
        final serviceId = service.uuid.toString().toLowerCase();
        debugPrint('[BLE] Service: $serviceId');
        
        if (serviceId.contains(serviceUuid.toLowerCase().replaceAll('-', ''))) {
          debugPrint('[BLE] Found our service!');
          
          for (var char in service.characteristics) {
            final charId = char.uuid.toString().toLowerCase();
            debugPrint('[BLE]   Characteristic: $charId');
            
            if (charId.contains(dataCharUuid.toLowerCase().replaceAll('-', ''))) {
              debugPrint('[BLE]   -> DATA characteristic');
              _dataChar = char;
              await char.setNotifyValue(true);
              _dataSubscription = char.onValueReceived.listen(_onDataReceived);
              debugPrint('[BLE]   -> Notify enabled for DATA');
            } else if (charId.contains(cmdCharUuid.toLowerCase().replaceAll('-', ''))) {
              debugPrint('[BLE]   -> CMD characteristic');
              _cmdChar = char;
            } else if (charId.contains(dbSyncCharUuid.toLowerCase().replaceAll('-', ''))) {
              debugPrint('[BLE]   -> DB_SYNC characteristic');
              _dbSyncChar = char;
              await char.setNotifyValue(true);
              _dbSyncSubscription = char.onValueReceived.listen(_onDbSyncReceived);
              debugPrint('[BLE]   -> Notify enabled for DB_SYNC');
            }
          }
        }
      }

      _isConnected = true;
      _setupConnectionListener();
      notifyListeners();
      debugPrint('[BLE] Connection setup complete');
      
      debugPrint('[BLE] Requesting profile list...');
      await requestProfileList();
      
      return true;
    } catch (e, stackTrace) {
      debugPrint('[BLE] ===== CONNECT ERROR =====');
      debugPrint('[BLE] Error: $e');
      debugPrint('[BLE] Stack: $stackTrace');
      await disconnect();
      return false;
    }
  }

  void _setupConnectionListener() {
    _connectionStateSubscription?.cancel();
    _connectionStateSubscription = _connectedDevice?.connectionState.listen((state) {
      debugPrint('[BLE] Connection state: $state');
      if (state == ble.BluetoothConnectionState.disconnected) {
        debugPrint('[BLE] Connection lost!');
        _isConnected = false;
        _holderData = null;
        notifyListeners();
      }
    });
  }

  Future<void> disconnect() async {
    _simulationTimer?.cancel();
    _simulationTimer = null;
    await _dataSubscription?.cancel();
    await _dbSyncSubscription?.cancel();
    await _scanSubscription?.cancel();
    await _connectionStateSubscription?.cancel();
    await _connectedDevice?.disconnect();
    _connectedDevice = null;
    _dataChar = null;
    _cmdChar = null;
    _dbSyncChar = null;
    _isConnected = false;
    _isSimulation = false;
    _holderData = null;
    _profiles.clear();
    _dbChunks.clear();
    _profileCompleter = null;
    notifyListeners();
  }

  // Режим симуляции для тестирования без реального устройства
  void startSimulation() {
    _isSimulation = true;
    _isConnected = true;
    
    _profiles.clear();
    _profiles = [
      Filament(
        id: '3d-fuel_pla+_1000.0_1.75',
        manufacturer: '3D-Fuel',
        material: 'PLA+',
        density: 1.22,
        weight: 1000,
        spoolWeight: 200,
        diameter: 1.75,
        bedTemp: 60,
      ),
      Filament(
        id: 'esun_petg_1000.0_1.75',
        manufacturer: 'eSUN',
        material: 'PETG',
        density: 1.27,
        weight: 1000,
        spoolWeight: 250,
        diameter: 1.75,
        bedTemp: 70,
      ),
      Filament(
        id: 'polymaker_abs_1000.0_1.75',
        manufacturer: 'Polymaker',
        material: 'ABS',
        density: 1.04,
        weight: 1000,
        spoolWeight: 200,
        diameter: 1.75,
        bedTemp: 100,
      ),
    ];
    
    double weight = 850.0;
    _simulationTimer = Timer.periodic(const Duration(seconds: 1), (timer) {
      weight += (DateTime.now().millisecond % 10 - 5) * 0.1;
      _holderData = HolderData(
        grossWeight: weight + 200,
        netWeight: weight,
        spoolWeight: 200,
        currentFilamentId: '3d-fuel_pla+_1000.0_1.75',
      );
      notifyListeners();
    });
    
    notifyListeners();
  }

  void _onDataReceived(List<int> value) {
    try {
      String jsonStr = utf8.decode(value);
      Map<String, dynamic> data = json.decode(jsonStr);
      _holderData = HolderData.fromJson(data);
      notifyListeners();
    } catch (e) {
      debugPrint('Parse error: $e');
    }
  }

  void _onDbSyncReceived(List<int> value) {
    try {
      debugPrint('[BLE] ===== DB SYNC RECEIVED =====');
      debugPrint('[BLE] Raw bytes length: ${value.length}');
      
      String jsonStr = utf8.decode(value);
      debugPrint('[BLE] Decoded string length: ${jsonStr.length} bytes');
      debugPrint('[BLE] First 500 chars: ${jsonStr.substring(0, jsonStr.length > 500 ? 500 : jsonStr.length)}');
      debugPrint('[BLE] Last 200 chars: ${jsonStr.length > 200 ? jsonStr.substring(jsonStr.length - 200) : "N/A"}');
      
      Map<String, dynamic> data = json.decode(jsonStr);
      debugPrint('[BLE] Parsed JSON successfully');
      debugPrint('[BLE] CMD: ${data['cmd']}');
      debugPrint('[BLE] Keys: ${data.keys.toList()}');
      
      if (data['cmd'] == 'database_chunk') {
        debugPrint('[BLE] Chunk ${data['index']}/${data['total']}');
        _dbChunks.add(data['data']);
        
        // Check if this is the last chunk
        if (data['index'] == data['total'] - 1) {
          debugPrint('[BLE] Last chunk received, assembling...');
          _assembleProfileList();
        }
      } else if (data['cmd'] == 'profile_list') {
        debugPrint('[BLE] Direct profile list received!');
        if (data.containsKey('profiles')) {
          debugPrint('[BLE] Profiles array length: ${(data['profiles'] as List).length}');
        } else {
          debugPrint('[BLE] ERROR: No profiles key in data!');
        }
        _parseProfileList(data);
      } else if (data['cmd'] == 'profile_data') {
        debugPrint('[BLE] Full profile data received');
        if (data['success'] == true && data['data'] != null) {
          final profile = Filament.fromJson(data['data']);
          _profileCompleter?.complete(profile);
        } else {
          _profileCompleter?.complete(null);
        }
      } else {
        debugPrint('[BLE] Unknown cmd: ${data['cmd']}');
      }
    } catch (e, stackTrace) {
      debugPrint('[BLE] ===== DB SYNC ERROR =====');
      debugPrint('[BLE] Error: $e');
      debugPrint('[BLE] Stack: $stackTrace');
      debugPrint('[BLE] Raw data length: ${value.length}');
      try {
        String jsonStr = utf8.decode(value);
        debugPrint('[BLE] Raw string: $jsonStr');
      } catch (e2) {
        debugPrint('[BLE] Cannot decode as UTF8: $e2');
      }
    }
  }

  void _assembleProfileList() {
    try {
      String fullJson = _dbChunks.join('');
      Map<String, dynamic> data = json.decode(fullJson);
      _parseProfileList(data);
      _dbChunks.clear();
    } catch (e) {
      debugPrint('Assemble error: $e');
      _isLoadingProfiles = false;
      notifyListeners();
    }
  }

  void _parseProfileList(Map<String, dynamic> data) {
    try {
      debugPrint('[BLE] ===== PARSING PROFILE LIST =====');
      _profiles.clear();
      
      List<dynamic> profilesData = data['profiles'] ?? [];
      debugPrint('[BLE] Found ${profilesData.length} profiles in data');
      
      for (var item in profilesData) {
        try {
          // Parse full profile data from ESP32
          final profile = Filament(
            id: item['id'] ?? '',
            manufacturer: item['manufacturer'] ?? '',
            material: item['material'] ?? '',
            density: (item['density'] ?? 1.0).toDouble(),
            weight: (item['weight'] ?? 0.0).toDouble(),
            spoolWeight: item['spool_weight']?.toDouble(),
            spoolType: item['spool_type'],
            diameter: (item['diameter'] ?? 1.75).toDouble(),
            bedTemp: item['bed_temp'],
            isCustom: item['is_custom'] ?? false,
          );
          _profiles.add(profile);
          
          if (_profiles.length <= 3) {
            debugPrint('[BLE] Profile ${_profiles.length}: ${profile.manufacturer} ${profile.material}');
          }
        } catch (e) {
          debugPrint('[BLE] Error parsing profile: $e');
          debugPrint('[BLE] Profile data: $item');
        }
      }
      
      debugPrint('[BLE] Successfully parsed ${_profiles.length} profiles');
      _isLoadingProfiles = false;
      
      debugPrint('[BLE] Calling notifyListeners()...');
      notifyListeners();
      debugPrint('[BLE] notifyListeners() called!');
    } catch (e, stackTrace) {
      debugPrint('[BLE] ===== PARSE ERROR =====');
      debugPrint('[BLE] Error: $e');
      debugPrint('[BLE] Stack: $stackTrace');
      _isLoadingProfiles = false;
      notifyListeners();
    }
  }

  Future<void> requestProfileList() async {
    if (_isSimulation) {
      notifyListeners();
      return;
    }
    
    if (_cmdChar == null) return;
    
    try {
      _isLoadingProfiles = true;
      _dbChunks.clear();
      notifyListeners();
      
      Map<String, dynamic> cmd = {'cmd': 'get_profile_list'};
      await _cmdChar!.write(utf8.encode(json.encode(cmd)), withoutResponse: false);
      
      Future.delayed(const Duration(seconds: 30), () {
        if (_isLoadingProfiles) {
          debugPrint('[BLE] Profile list timeout!');
          _isLoadingProfiles = false;
          notifyListeners();
        }
      });
    } catch (e) {
      debugPrint('Request profile list error: $e');
      _isLoadingProfiles = false;
      notifyListeners();
    }
  }

  Future<Filament?> requestFullProfile(String id) async {
    if (_isSimulation) {
      return _profiles.firstWhere((p) => p.id == id, orElse: () => _profiles.first);
    }
    
    if (_cmdChar == null) return null;
    
    try {
      _profileCompleter = Completer<Filament?>();
      
      Map<String, dynamic> cmd = {'cmd': 'get_profile', 'id': id};
      await _cmdChar!.write(utf8.encode(json.encode(cmd)), withoutResponse: false);
      
      return await _profileCompleter!.future.timeout(
        const Duration(seconds: 5),
        onTimeout: () => null,
      );
    } catch (e) {
      debugPrint('Request full profile error: $e');
      return null;
    }
  }

  Future<bool> addCustomProfile(Filament profile) async {
    if (_isSimulation) {
      _profiles.add(profile);
      notifyListeners();
      return true;
    }
    
    if (_cmdChar == null) return false;
    
    try {
      Map<String, dynamic> cmd = {
        'cmd': 'add_profile',
        'data': profile.toJson(),
      };
      await _cmdChar!.write(utf8.encode(json.encode(cmd)), withoutResponse: false);
      
      // Add to local list immediately
      _profiles.add(profile);
      notifyListeners();
      
      return true;
    } catch (e) {
      debugPrint('Add profile error: $e');
      return false;
    }
  }

  Future<bool> writeNFC(String profileId) async {
    if (_isSimulation) return true;
    if (_cmdChar == null) return false;
    
    try {
      Map<String, dynamic> cmd = {
        'cmd': 'write_nfc',
        'id': profileId,
      };
      await _cmdChar!.write(utf8.encode(json.encode(cmd)), withoutResponse: false);
      return true;
    } catch (e) {
      debugPrint('Write NFC error: $e');
      return false;
    }
  }

  Future<bool> sendFilamentProfile(Filament filament) async {
    if (_isSimulation) return true; // В симуляции просто возвращаем успех
    if (_cmdChar == null) return false;
    
    try {
      Map<String, dynamic> cmd = {
        'cmd': 'set_profile',
        'data': filament.toJson(),
      };
      await _cmdChar!.write(utf8.encode(json.encode(cmd)));
      return true;
    } catch (e) {
      debugPrint('Send error: $e');
      return false;
    }
  }

  @override
  void dispose() {
    disconnect();
    super.dispose();
  }
}
