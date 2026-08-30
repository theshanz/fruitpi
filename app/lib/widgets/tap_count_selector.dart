import 'package:flutter/material.dart';

/// Selector for the number of taps per inference run (N-tap consensus). The
/// choice is pushed to the firmware over BLE (`tap_count`); firmware fuses the
/// per-tap posteriors with a median before deciding.
class TapCountSelector extends StatelessWidget {
  final int tapCount;
  final ValueChanged<int> onChanged;

  const TapCountSelector({
    super.key,
    required this.tapCount,
    required this.onChanged,
  });

  static const _options = <(int, String)>[
    (1, '1 Tap (Fast)'),
    (3, '3 Taps (Balanced)'),
    (5, '5 Taps (High Precision)'),
  ];

  @override
  Widget build(BuildContext context) {
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      const Text('N-TAP CONSENSUS',
          style: TextStyle(fontSize: 14, letterSpacing: 1, color: Colors.white70)),
      const SizedBox(height: 6),
      Wrap(
        spacing: 8,
        runSpacing: 8,
        children: [
          for (final (n, label) in _options)
            ChoiceChip(
              label: Text(label, style: const TextStyle(fontSize: 15)),
              selected: tapCount == n,
              onSelected: (_) => onChanged(n),
              selectedColor: Colors.blueAccent,
              backgroundColor: Colors.black26,
              shape: RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(14)),
            ),
        ],
      ),
    ]);
  }
}
