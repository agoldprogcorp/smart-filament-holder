import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../models/filament.dart';
import '../services/bluetooth_service.dart';
import '../services/database_service.dart';

class FilamentListScreen extends StatefulWidget {
  const FilamentListScreen({super.key});

  @override
  State<FilamentListScreen> createState() => _FilamentListScreenState();
}

class _FilamentListScreenState extends State<FilamentListScreen> {
  final _searchController = TextEditingController();
  String? _selectedMaterial;
  String? _selectedManufacturer;
  List<Filament> _filteredFilaments = [];

  @override
  void initState() {
    super.initState();
    // Don't load immediately - wait for profiles to be loaded from BLE
    // _loadFilaments() will be called automatically when bt.profiles changes
    // because we use context.watch<BluetoothService>() in build()
  }

  void _loadFilaments() {
    final bt = context.read<BluetoothService>();
    final db = context.read<DatabaseService>();
    final allFilaments = bt.profiles;
    
    debugPrint('[UI] Loading filaments: ${allFilaments.length} total');
    
    setState(() {
      _filteredFilaments = db.searchFilaments(
        allFilaments,
        _searchController.text,
        material: _selectedMaterial,
        manufacturer: _selectedManufacturer,
      );
    });
    
    debugPrint('[UI] Filtered filaments: ${_filteredFilaments.length}');
  }

  @override
  Widget build(BuildContext context) {
    final bt = context.watch<BluetoothService>();
    final db = context.watch<DatabaseService>();
    final allFilaments = bt.profiles;
    
    // Auto-update filtered list when profiles change
    if (allFilaments.isNotEmpty && _filteredFilaments.isEmpty) {
      debugPrint('[UI] Auto-loading filaments because profiles changed');
      WidgetsBinding.instance.addPostFrameCallback((_) => _loadFilaments());
    }
    
    return Scaffold(
      appBar: AppBar(
        title: const Text('Филаменты'),
        actions: [
          IconButton(
            icon: const Icon(Icons.add),
            onPressed: () => _showCreateDialog(context),
          ),
          IconButton(
            icon: const Icon(Icons.refresh),
            onPressed: bt.isConnected ? () async {
              await bt.requestProfileList();
              _loadFilaments();
            } : null,
          ),
        ],
      ),
      body: bt.isLoadingProfiles
        ? const Center(child: CircularProgressIndicator())
        : !bt.isConnected
          ? const Center(
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Icon(Icons.bluetooth_disabled, size: 64, color: Colors.grey),
                  SizedBox(height: 16),
                  Text('Подключитесь к держателю для просмотра профилей'),
                ],
              ),
            )
          : Column(
              children: [
                Padding(
                  padding: const EdgeInsets.all(8),
                  child: TextField(
                    controller: _searchController,
                    decoration: InputDecoration(
                      hintText: 'Поиск...',
                      prefixIcon: const Icon(Icons.search),
                      border: OutlineInputBorder(borderRadius: BorderRadius.circular(12)),
                    ),
                    onChanged: (_) => _loadFilaments(),
                  ),
                ),
                Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 8),
                  child: Row(
                    children: [
                      Expanded(
                        child: DropdownButtonFormField<String>(
                          value: _selectedMaterial,
                          decoration: const InputDecoration(
                            labelText: 'Материал',
                            contentPadding: EdgeInsets.symmetric(horizontal: 12),
                          ),
                          items: [
                            const DropdownMenuItem(value: null, child: Text('Все')),
                            ...db.getUniqueMaterials(allFilaments).map((m) => 
                              DropdownMenuItem(value: m, child: Text(m))),
                          ],
                          onChanged: (v) {
                            setState(() => _selectedMaterial = v);
                            _loadFilaments();
                          },
                        ),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: DropdownButtonFormField<String>(
                          value: _selectedManufacturer,
                          decoration: const InputDecoration(
                            labelText: 'Производитель',
                            contentPadding: EdgeInsets.symmetric(horizontal: 12),
                          ),
                          items: [
                            const DropdownMenuItem(value: null, child: Text('Все')),
                            ...db.getUniqueManufacturers(allFilaments).map((m) => 
                              DropdownMenuItem(value: m, child: Text(m))),
                          ],
                          onChanged: (v) {
                            setState(() => _selectedManufacturer = v);
                            _loadFilaments();
                          },
                        ),
                      ),
                    ],
                  ),
                ),
                const SizedBox(height: 8),
                Expanded(
                  child: _filteredFilaments.isEmpty
                    ? const Center(child: Text('Профили не найдены'))
                    : ListView.builder(
                        itemCount: _filteredFilaments.length,
                        itemBuilder: (context, index) {
                          final filament = _filteredFilaments[index];
                          return _FilamentTile(filament: filament);
                        },
                      ),
                ),
              ],
            ),
    );
  }

  void _showCreateDialog(BuildContext context) {
    final bt = context.read<BluetoothService>();
    
    if (!bt.isConnected) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Подключитесь к держателю для создания профиля')),
      );
      return;
    }
    
    final formKey = GlobalKey<FormState>();
    String manufacturer = '';
    String material = '';
    double density = 1.24;
    double weight = 1000;
    double spoolWeight = 200;
    double diameter = 1.75;
    int bedTemp = 60;

    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Новый филамент'),
        content: SingleChildScrollView(
          child: Form(
            key: formKey,
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                TextFormField(
                  decoration: const InputDecoration(labelText: 'Производитель'),
                  validator: (v) => v?.isEmpty ?? true ? 'Обязательно' : null,
                  onSaved: (v) => manufacturer = v ?? '',
                ),
                TextFormField(
                  decoration: const InputDecoration(labelText: 'Материал'),
                  validator: (v) => v?.isEmpty ?? true ? 'Обязательно' : null,
                  onSaved: (v) => material = v ?? '',
                ),
                TextFormField(
                  decoration: const InputDecoration(labelText: 'Плотность (г/см³)'),
                  keyboardType: TextInputType.number,
                  initialValue: '1.24',
                  onSaved: (v) => density = double.tryParse(v ?? '') ?? 1.24,
                ),
                TextFormField(
                  decoration: const InputDecoration(labelText: 'Вес филамента (г)'),
                  keyboardType: TextInputType.number,
                  initialValue: '1000',
                  onSaved: (v) => weight = double.tryParse(v ?? '') ?? 1000,
                ),
                TextFormField(
                  decoration: const InputDecoration(labelText: 'Вес катушки (г)'),
                  keyboardType: TextInputType.number,
                  initialValue: '200',
                  onSaved: (v) => spoolWeight = double.tryParse(v ?? '') ?? 200,
                ),
                TextFormField(
                  decoration: const InputDecoration(labelText: 'Диаметр (мм)'),
                  keyboardType: TextInputType.number,
                  initialValue: '1.75',
                  onSaved: (v) => diameter = double.tryParse(v ?? '') ?? 1.75,
                ),
                TextFormField(
                  decoration: const InputDecoration(labelText: 'Температура стола (°C)'),
                  keyboardType: TextInputType.number,
                  initialValue: '60',
                  onSaved: (v) => bedTemp = int.tryParse(v ?? '') ?? 60,
                ),
              ],
            ),
          ),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: const Text('Отмена'),
          ),
          ElevatedButton(
            onPressed: () async {
              if (formKey.currentState?.validate() ?? false) {
                formKey.currentState?.save();
                final id = 'custom_${manufacturer.toLowerCase()}_${material.toLowerCase()}_${weight}_$diameter';
                final filament = Filament(
                  id: id,
                  manufacturer: manufacturer,
                  material: material,
                  density: density,
                  weight: weight,
                  spoolWeight: spoolWeight,
                  diameter: diameter,
                  bedTemp: bedTemp,
                  isCustom: true,
                );
                
                final success = await bt.addCustomProfile(filament);
                if (ctx.mounted) Navigator.pop(ctx);
                if (context.mounted) {
                  ScaffoldMessenger.of(context).showSnackBar(
                    SnackBar(content: Text(success ? 'Профиль создан и отправлен на держатель' : 'Ошибка создания профиля')),
                  );
                  if (success) _loadFilaments();
                }
              }
            },
            child: const Text('Создать'),
          ),
        ],
      ),
    );
  }
}

class _FilamentTile extends StatefulWidget {
  final Filament filament;

  const _FilamentTile({required this.filament});

  @override
  State<_FilamentTile> createState() => _FilamentTileState();
}

class _FilamentTileState extends State<_FilamentTile> {
  bool _isLoadingDetails = false;
  Filament? _fullFilament;

  Filament get _displayFilament => _fullFilament ?? widget.filament;

  @override
  Widget build(BuildContext context) {
    return Card(
      margin: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      child: ListTile(
        leading: CircleAvatar(
          child: Text(widget.filament.material.substring(0, widget.filament.material.length.clamp(0, 2))),
        ),
        title: Text('${widget.filament.manufacturer} - ${widget.filament.material}'),
        subtitle: Text(
          '${widget.filament.weight.toStringAsFixed(0)}г | Ø${widget.filament.diameter}мм',
        ),
        trailing: widget.filament.isCustom
          ? const Icon(Icons.star, color: Colors.amber)
          : widget.filament.isPartial
            ? const Icon(Icons.cloud_download, size: 18, color: Colors.grey)
            : null,
        onTap: () => _showDetails(context),
      ),
    );
  }

  Future<void> _loadFullProfile() async {
    if (!widget.filament.isPartial || _fullFilament != null) return;

    final bt = context.read<BluetoothService>();
    if (!bt.isConnected) return;

    setState(() => _isLoadingDetails = true);

    final full = await bt.requestFullProfile(widget.filament.id);
    if (full != null && mounted) {
      setState(() {
        _fullFilament = widget.filament.copyWithFullData(full);
        _isLoadingDetails = false;
      });
    } else if (mounted) {
      setState(() => _isLoadingDetails = false);
    }
  }

  void _showDetails(BuildContext context) {
    // Загружаем полные данные при открытии
    if (widget.filament.isPartial && _fullFilament == null) {
      _loadFullProfile();
    }

    showModalBottomSheet(
      context: context,
      builder: (ctx) => StatefulBuilder(
        builder: (context, setSheetState) {
          final filament = _displayFilament;

          return Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  '${filament.manufacturer} - ${filament.material}',
                  style: Theme.of(context).textTheme.titleLarge,
                ),
                const SizedBox(height: 16),
                if (_isLoadingDetails)
                  const Center(child: CircularProgressIndicator())
                else ...[
                  _infoRow('ID', filament.id),
                  _infoRow('Плотность', '${filament.density} г/см³'),
                  _infoRow('Вес филамента', '${filament.weight.toStringAsFixed(0)} г'),
                  _infoRow('Вес катушки', '${filament.spoolWeight?.toStringAsFixed(0) ?? "-"} г'),
                  _infoRow('Диаметр', '${filament.diameter} мм'),
                  _infoRow('Температура стола', '${filament.bedTemp ?? "-"} °C'),
                ],
                const SizedBox(height: 16),
                SizedBox(
                  width: double.infinity,
                  child: ElevatedButton.icon(
                    onPressed: _isLoadingDetails ? null : () async {
                      final bt = context.read<BluetoothService>();
                      if (!bt.isConnected) {
                        ScaffoldMessenger.of(context).showSnackBar(
                          const SnackBar(content: Text('Сначала подключитесь к устройству')),
                        );
                        return;
                      }

                      // Загружаем полные данные если профиль частичный
                      var profileToSend = _displayFilament;
                      if (profileToSend.isPartial) {
                        final full = await bt.requestFullProfile(profileToSend.id);
                        if (full != null) {
                          profileToSend = profileToSend.copyWithFullData(full);
                        } else {
                          if (context.mounted) {
                            ScaffoldMessenger.of(context).showSnackBar(
                              const SnackBar(content: Text('Не удалось загрузить полные данные профиля')),
                            );
                          }
                          return;
                        }
                      }

                      final success = await bt.sendFilamentProfile(profileToSend);
                      if (ctx.mounted) Navigator.pop(ctx);
                      if (context.mounted) {
                        ScaffoldMessenger.of(context).showSnackBar(
                          SnackBar(content: Text(success ? 'Профиль отправлен' : 'Ошибка отправки')),
                        );
                      }
                    },
                    icon: const Icon(Icons.send),
                    label: const Text('Отправить на держатель'),
                  ),
                ),
                const SizedBox(height: 8),
                SizedBox(
                  width: double.infinity,
                  child: OutlinedButton.icon(
                    onPressed: _isLoadingDetails ? null : () => _writeToNfc(context, ctx),
                    icon: const Icon(Icons.nfc),
                    label: const Text('Записать на NFC метку'),
                  ),
                ),
              ],
            ),
          );
        },
      ),
    );
  }

  Widget _infoRow(String label, String value) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Text(label, style: const TextStyle(color: Colors.grey)),
          Text(value),
        ],
      ),
    );
  }

  void _writeToNfc(BuildContext context, BuildContext bottomSheetContext) async {
    final bt = context.read<BluetoothService>();
    final filament = _displayFilament;

    if (!bt.isConnected) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Подключитесь к держателю для записи на NFC')),
      );
      return;
    }

    // Закрываем bottom sheet
    if (bottomSheetContext.mounted) {
      Navigator.pop(bottomSheetContext);
    }

    // Отправляем команду записи
    final success = await bt.writeNFC(filament.id);

    if (!success) {
      if (context.mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(
            content: Text('Ошибка отправки команды'),
            backgroundColor: Colors.red,
          ),
        );
      }
      return;
    }

    // Показываем диалог ожидания записи
    if (context.mounted) {
      showDialog(
        context: context,
        barrierDismissible: true,
        builder: (dialogCtx) => AlertDialog(
          title: const Text('Запись NFC'),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              const Icon(Icons.nfc, size: 64, color: Colors.blue),
              const SizedBox(height: 16),
              const Text(
                'Приложите NFC метку к держателю',
                textAlign: TextAlign.center,
              ),
              const SizedBox(height: 8),
              Text(
                '${filament.manufacturer} - ${filament.material}',
                style: const TextStyle(fontWeight: FontWeight.bold),
                textAlign: TextAlign.center,
              ),
              const SizedBox(height: 16),
              const LinearProgressIndicator(),
            ],
          ),
          actions: [
            TextButton(
              onPressed: () {
                if (dialogCtx.mounted) Navigator.pop(dialogCtx);
              },
              child: const Text('Закрыть'),
            ),
          ],
        ),
      );
    }

    // Даём время на запись (5 секунд)
    await Future.delayed(const Duration(seconds: 5));

    // Закрываем диалог и показываем результат
    if (context.mounted) {
      Navigator.of(context, rootNavigator: true).pop();
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text('Запись завершена. Проверьте NFC метку.'),
          backgroundColor: Colors.green,
          duration: Duration(seconds: 3),
        ),
      );
    }
  }
}
