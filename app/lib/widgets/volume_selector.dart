import 'package:flutter/material.dart';

/// Size-tier selector for the fruit volume used during a scan run. The chosen
/// volume is pushed to the firmware as a run-wide override over BLE
/// (`volume_cm3`), and the Dim 9 mass factor (volume^(2/3) norm) is previewed
/// live.
class VolumeSelector extends StatelessWidget {
  final double volumeCm3;
  final ValueChanged<double> onChanged;

  const VolumeSelector({
    super.key,
    required this.volumeCm3,
    required this.onChanged,
  });

  static const _tiers = <(String, double)>[
    ('Small', 180.0),
    ('Medium', 350.0),
    ('Large', 520.0),
  ];

  static const _min = 50.0;
  static const _max = 600.0;

  static double _massFactor(double volCm3) {
    final vol = volCm3.clamp(10.0, 600.0);
    final z = (vol - 10.0) / (600.0 - 10.0); // 0..1, mirrors firmware dim 9
    return z;
  }

  @override
  Widget build(BuildContext context) {
    final mass = _massFactor(volumeCm3);
    final tier = _tiers.firstWhere(
      (t) => (t.$2 - volumeCm3).abs() < 40,
      orElse: () => ('Custom', volumeCm3),
    );

    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Row(children: [
        const Expanded(
          child: Text('READ SCAN VOLUME',
              style: TextStyle(
                  fontSize: 14, letterSpacing: 1, color: Colors.white70)),
        ),
        Text('${volumeCm3.round()} cm³',
            style: const TextStyle(
                fontFamily: 'monospace', fontSize: 16, color: Colors.white)),
      ]),
      const SizedBox(height: 6),
      Row(children: [
        for (final (label, value) in _tiers)
          Padding(
            padding: const EdgeInsets.only(right: 8),
            child: ChoiceChip(
              label: Text('$label (${value.round()})',
                  style: const TextStyle(fontSize: 15)),
              selected: tier.$2 == value,
              onSelected: (_) => onChanged(value),
              selectedColor: Colors.green,
              backgroundColor: Colors.black26,
              shape: RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(14)),
            ),
          ),
      ]),
      Slider(
        value: volumeCm3.clamp(_min, _max),
        min: _min,
        max: _max,
        activeColor: Colors.green,
        onChanged: onChanged,
      ),
      Row(children: [
        const Text('Mass factor (Dim 9):',
            style: TextStyle(fontSize: 14, color: Colors.white70)),
        const SizedBox(width: 6),
        Text(mass.toStringAsFixed(3),
            style: const TextStyle(
                fontFamily: 'monospace', fontSize: 15, color: Colors.white)),
      ]),
    ]);
  }
}
