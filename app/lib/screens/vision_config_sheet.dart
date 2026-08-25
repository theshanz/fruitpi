import 'package:flutter/material.dart';

import '../core/cozy_palette.dart';
import '../services/ble_service.dart';
import '../widgets/frosted.dart';

/// Live tuning of the camera's black-spot / glare rejection gates.
/// Sends `vision_config` over BLE; device applies immediately (session-only,
/// resets on ESP reboot).
Future<void> showVisionConfigSheet(BuildContext context, BleService ble) {
  double valueMin = 0.15; // black sensitivity
  double satMin = 0.15; // glare sensitivity

  return showModalBottomSheet(
    context: context,
    isScrollControlled: true,
    backgroundColor: Cozy.espresso.withValues(alpha: 0.98),
    builder: (ctx) => StatefulBuilder(
      builder: (ctx, setLocal) => Padding(
        padding: EdgeInsets.only(
            bottom: MediaQuery.of(ctx).viewInsets.bottom + 20,
            left: 18,
            right: 18,
            top: 8),
        child: FrostedBox(
          borderRadius: BorderRadius.circular(24),
          backgroundColor: Colors.transparent,
          child: Column(mainAxisSize: MainAxisSize.min, children: [
            const Align(
              alignment: Alignment.centerLeft,
              child:
                  SectionLabel(title: '// VISION GATES · LIVE ON DEVICE'),
            ),
            const SizedBox(height: 6),

            _gateSlider(
              label: 'BLACK SPOT GATE (value_min)',
              hint:
                  'pixels darker than this are ignored · raise to tolerate more dark spots',
              value: valueMin,
              onChanged: (v) {
                setLocal(() => valueMin = v);
                ble.sendVisionConfig(valueMin: v); // stream live
              },
              color: Cozy.chamomile,
            ),
            _gateSlider(
              label: 'GLARE GATE (sat_min)',
              hint:
                  'washed-out pixels below this are ignored · raise under harsh light',
              value: satMin,
              onChanged: (v) {
                setLocal(() => satMin = v);
                ble.sendVisionConfig(satMin: v);
              },
              color: Cozy.duskBlue,
            ),
            const SizedBox(height: 8),
            Text(
              'applied instantly over BLE · SAVE & CLOSE writes them to flash '
              '(NVS) so they survive ESP reboots.',
              style: TextStyle(fontSize: 9.5, height: 1.5, color: Cozy.dimGray),
            ),
            const SizedBox(height: 10),
            Row(mainAxisAlignment: MainAxisAlignment.end, children: [
              TextButton(
                onPressed: () {
                  setLocal(() {
                    valueMin = 0.15;
                    satMin = 0.15;
                  });
                  ble.sendVisionConfig(
                      valueMin: 0.15, satMin: 0.15, save: true);
                },
                child: const Text('RESET DEFAULTS'),
              ),
              const SizedBox(width: 8),
              FilledButton(
                style: FilledButton.styleFrom(
                    backgroundColor: Cozy.matcha,
                    foregroundColor: Cozy.deepBg),
                onPressed: () {
                  // persist current gates to ESP NVS so reboots keep them
                  ble.sendVisionConfig(
                      valueMin: valueMin, satMin: satMin, save: true);
                  Navigator.pop(ctx);
                },
                child: const Text('SAVE & CLOSE'),
              ),
            ]),
          ]),
        ),
      ),
    ),
  );
}

Widget _gateSlider({
  required String label,
  required String hint,
  required double value,
  required ValueChanged<double> onChanged,
  required Color color,
}) {
  return Padding(
    padding: const EdgeInsets.symmetric(vertical: 6),
    child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Row(children: [
        Expanded(
            child: Text(label,
                style: TextStyle(
                    fontSize: 11,
                    fontWeight: FontWeight.bold,
                    letterSpacing: 0.5,
                    color: color))),
        Text(value.toStringAsFixed(2),
            style: TextStyle(fontSize: 11, color: Cozy.oatmeal)),
      ]),
      SliderTheme(
        data: SliderThemeData(
          trackHeight: 5,
          activeTrackColor: color.withValues(alpha: 0.85),
          inactiveTrackColor: Colors.white.withValues(alpha: 0.08),
          thumbColor: color,
          overlayColor: color.withValues(alpha: 0.15),
          thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 8),
          overlayShape: const RoundSliderOverlayShape(overlayRadius: 14),
        ),
        child: Slider(
          value: value,
          min: 0.0,
          max: 0.9,
          divisions: 90,
          onChanged: onChanged,
        ),
      ),
      Text(hint,
          style: TextStyle(fontSize: 9.5, height: 1.4, color: Cozy.dimGray)),
    ]),
  );
}
