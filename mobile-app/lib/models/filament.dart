class Filament {
  final String id;
  final String manufacturer;
  final String material;
  final double density;
  final double weight;
  final double? spoolWeight;
  final String? spoolType;
  final double diameter;
  final int? bedTemp;
  final bool isCustom;
  final bool isPartial;  // true если загружена только краткая информация

  Filament({
    required this.id,
    required this.manufacturer,
    required this.material,
    required this.density,
    required this.weight,
    this.spoolWeight,
    this.spoolType,
    required this.diameter,
    this.bedTemp,
    this.isCustom = false,
    this.isPartial = false,
  });

  factory Filament.fromJson(Map<String, dynamic> json) {
    // Проверяем есть ли полные данные (density, spool_weight)
    final bool partial = !json.containsKey('density') || !json.containsKey('spool_weight');

    return Filament(
      id: json['id'] ?? '',
      manufacturer: json['manufacturer'] ?? '',
      material: json['material'] ?? '',
      density: (json['density'] ?? 1.24).toDouble(),  // default PLA density
      weight: (json['weight'] ?? 0.0).toDouble(),
      spoolWeight: json['spool_weight']?.toDouble(),
      spoolType: json['spool_type'],
      diameter: (json['diameter'] ?? 1.75).toDouble(),
      bedTemp: json['bed_temp'],
      isCustom: json['is_custom'] ?? false,
      isPartial: partial,
    );
  }

  /// Создаёт полный профиль из частичного + данные с ESP32
  Filament copyWithFullData(Filament fullData) {
    return Filament(
      id: id,
      manufacturer: manufacturer,
      material: material,
      density: fullData.density,
      weight: weight,
      spoolWeight: fullData.spoolWeight,
      spoolType: fullData.spoolType,
      diameter: fullData.diameter,
      bedTemp: fullData.bedTemp,
      isCustom: isCustom,
      isPartial: false,
    );
  }

  Map<String, dynamic> toJson() => {
    'id': id,
    'manufacturer': manufacturer,
    'material': material,
    'density': density,
    'weight': weight,
    'spool_weight': spoolWeight,
    'spool_type': spoolType,
    'diameter': diameter,
    'bed_temp': bedTemp,
    'is_custom': isCustom,
  };

  Map<String, dynamic> toMap() => toJson();

  factory Filament.fromMap(Map<String, dynamic> map) => Filament.fromJson(map);
}
