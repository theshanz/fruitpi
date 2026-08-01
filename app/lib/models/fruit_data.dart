/// What the ESP32 sends over BLE after scanning a fruit.
class FruitData {
  final double ripenessScore; // 0-100
  final FruitStatus status;
  final String fruitType; // e.g. "mango", "apple"
  final String size; // "small", "medium", "large"

  const FruitData({
    required this.ripenessScore,
    required this.status,
    this.fruitType = '',
    this.size = '',
  });

  /// Parse raw bytes from the ESP32 BLE characteristic.
  ///
  /// Expected format (6 bytes):
  ///   [0]     = ripeness score (0-100)
  ///   [1]     = status (0=unripe, 1=ripe, 2=overripe, 3=rotten)
  ///   [2]     = fruit type index (0=mango, 1=apple, 2=banana, 3=other)
  ///   [3]     = size index (0=small, 1=medium, 2=large)
  ///   [4..5]  = reserved
  factory FruitData.fromBytes(List<int> bytes) {
    if (bytes.isEmpty) return FruitData.empty();

    final score = bytes[0].clamp(0, 100).toDouble();
    final statusIdx = bytes.length > 1 ? bytes[1].clamp(0, 4) : 0;
    final fruitIdx = bytes.length > 2 ? bytes[2].clamp(0, 3) : 3;
    final sizeIdx = bytes.length > 3 ? bytes[3].clamp(0, 2) : 1;

    const fruits = ['Mango', 'Apple', 'Banana', 'Other'];
    const sizes = ['Small', 'Medium', 'Large'];

    return FruitData(
      ripenessScore: score,
      status: FruitStatus.values[statusIdx],
      fruitType: fruits[fruitIdx],
      size: sizes[sizeIdx],
    );
  }

  factory FruitData.empty() => const FruitData(
        ripenessScore: 0,
        status: FruitStatus.unknown,
      );

  bool get isEmpty => status == FruitStatus.unknown;
}

enum FruitStatus {
  unripe,
  ripe,
  overripe,
  rotten,
  scanning,
  unknown;

  String get label {
    switch (this) {
      case FruitStatus.unripe:
        return 'Unripe';
      case FruitStatus.ripe:
        return 'Fresh';
      case FruitStatus.overripe:
        return 'Overripe';
      case FruitStatus.rotten:
        return 'Rotten';
      case FruitStatus.scanning:
        return 'Scanning...';
      case FruitStatus.unknown:
        return 'No Data';
    }
  }

  IconData get icon {
    switch (this) {
      case FruitStatus.unripe:
        return Icons.hourglass_empty_rounded;
      case FruitStatus.ripe:
        return Icons.check_circle_rounded;
      case FruitStatus.overripe:
        return Icons.warning_rounded;
      case FruitStatus.rotten:
        return Icons.do_not_disturb_on_rounded;
      case FruitStatus.scanning:
        return Icons.sensors_rounded;
      case FruitStatus.unknown:
        return Icons.help_outline_rounded;
    }
  }
}
