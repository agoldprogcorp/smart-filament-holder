import 'package:flutter/foundation.dart';
import '../models/filament.dart';

class DatabaseService extends ChangeNotifier {
  final List<Filament> _customFilaments = [];

  List<Filament> get customFilaments => _customFilaments;

  Future<List<Filament>> getCustomFilaments() async {
    return _customFilaments;
  }

  Future<void> saveCustomFilament(Filament filament) async {
    _customFilaments.add(filament);
    notifyListeners();
  }

  Future<void> deleteCustomFilament(String id) async {
    _customFilaments.removeWhere((f) => f.id == id);
    notifyListeners();
  }

  List<Filament> searchFilaments(List<Filament> allFilaments, String query, {String? material, String? manufacturer}) {
    return allFilaments.where((f) {
      bool matches = true;
      if (query.isNotEmpty) {
        matches = f.manufacturer.toLowerCase().contains(query.toLowerCase()) ||
                  f.material.toLowerCase().contains(query.toLowerCase()) ||
                  f.id.toLowerCase().contains(query.toLowerCase());
      }
      if (material != null && material.isNotEmpty) {
        matches = matches && f.material.toLowerCase() == material.toLowerCase();
      }
      if (manufacturer != null && manufacturer.isNotEmpty) {
        matches = matches && f.manufacturer.toLowerCase() == manufacturer.toLowerCase();
      }
      return matches;
    }).toList();
  }

  List<String> getUniqueMaterials(List<Filament> allFilaments) {
    return allFilaments.map((f) => f.material).toSet().toList()..sort();
  }

  List<String> getUniqueManufacturers(List<Filament> allFilaments) {
    return allFilaments.map((f) => f.manufacturer).toSet().toList()..sort();
  }
}
