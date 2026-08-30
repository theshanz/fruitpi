import 'package:flutter/material.dart';

import '../core/cozy_palette.dart';

/// Compact 4x4 time-frequency heat grid with a colormap + frequency & time
/// labels. The 16 cells are the compiled image block (dims 10..25); they are
/// unit-prototype magnitudes, so we contrast-normalise by the row max before
/// colouring, otherwise near-zero cells render as flat grey.
class SpectrogramGrid extends StatelessWidget {
  final List<double> values;
  final Color color;
  const SpectrogramGrid({super.key, required this.values, required this.color});

  static const _rowLbls = ['250', '500', '850', '1.4k']; // band 0..3, top->bottom
  static const _cellBg = Color(0xFF161B22);
  static const _blue = Color(0xFF1F6FEB);
  static const _amber = Color(0xFFD29922);
  static const _green = Color(0xFF7EE787);

  static Color _colormap(double t) {
    // 0.0 bg -> 0.3 blue -> 0.7 amber -> 1.0 green
    if (t < 0.3) {
      final k = t / 0.3;
      return Color.lerp(_cellBg, _blue, k)!;
    } else if (t < 0.7) {
      final k = (t - 0.3) / 0.4;
      return Color.lerp(_blue, _amber, k)!;
    } else {
      final k = (t - 0.7) / 0.3;
      return Color.lerp(_amber, _green, k)!;
    }
  }

  @override
  Widget build(BuildContext context) {
    final cells = List<double>.from(values);
    var maxV = 0.0;
    for (final v in cells) {
      final c = v.clamp(0.0, 1.0);
      if (c > maxV) maxV = c;
    }
    if (maxV <= 0) maxV = 1.0;

    // rows: top = band 3 (high freq), bottom = band 0.
    Widget row(int band, List<double> rowVals) {
      return Expanded(
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            SizedBox(
                width: 30,
                child: Text(_rowLbls[band],
                    style: const TextStyle(
                        fontSize: 7.5, color: Cozy.dimGray))),
            for (var t = 0; t < 4; t++)
              Expanded(
                child: Padding(
                  padding: const EdgeInsets.all(1.5),
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      color: _colormap(rowVals[t] / maxV),
                      borderRadius: BorderRadius.circular(4),
                    ),
                  ),
                ),
              ),
          ],
        ),
      );
    }

    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        Row(children: [
          const SizedBox(width: 30),
          for (var t = 0; t < 4; t++)
            Expanded(
              child: Text('T$t',
                  textAlign: TextAlign.center,
                  style: const TextStyle(fontSize: 7.5, color: Cozy.dimGray)),
            ),
        ]),
        const SizedBox(height: 2),
        SizedBox(
          height: 72,
          child: Column(children: [
            for (var band = 0; band < 4; band++)
              Expanded(
                child:
                    row(band, [for (var t = 0; t < 4; t++) cells[t * 4 + band]]),
              ),
          ]),
        ),
      ],
    );
  }
}

/// 6 bio-moment bars with value labels, 0.5/1.0 reference lines and a thin
/// baseline so a 0.0 bar is visibly "measured at zero" rather than missing.
class FleshDynamicsBars extends StatelessWidget {
  final List<double> values;
  final Color color;
  final List<String> labels;
  const FleshDynamicsBars(
      {super.key, required this.values, required this.color, required this.labels});

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        Row(children: [
          for (var i = 0; i < values.length; i++)
            Expanded(
              child: Text(values[i].clamp(0.0, 1.0).toStringAsFixed(2),
                  textAlign: TextAlign.center,
                  style: const TextStyle(
                      fontSize: 7.5,
                      fontWeight: FontWeight.bold,
                      color: Cozy.oatmeal)),
            ),
        ]),
        const SizedBox(height: 2),
        SizedBox(
          height: 52,
          child: Stack(children: [
            // reference lines at the 1.0 (top) and 0.5 (middle) envelopes
            Align(
              alignment: const Alignment(0, -1),
              child: Container(height: 1,
                  color: Colors.white10),
            ),
            Align(
              alignment: const Alignment(0, 0),
              child: Container(height: 1,
                  color: Colors.white10),
            ),
            Row(
              crossAxisAlignment: CrossAxisAlignment.end,
              children: [
                for (var i = 0; i < values.length; i++)
                  Expanded(
                    child: Align(
                      alignment: Alignment.bottomCenter,
                      child: Container(
                        width: 14,
                        // +2 keeps a thin baseline visible even for a 0.0 bar
                        height: 52 * values[i].clamp(0.0, 1.0) + 2,
                        decoration: BoxDecoration(
                          color: color.withValues(alpha: 0.85),
                          borderRadius: const BorderRadius.vertical(
                              top: Radius.circular(3)),
                        ),
                      ),
                    ),
                  ),
              ],
            ),
          ]),
        ),
        const SizedBox(height: 2),
        Row(children: [
          for (var i = 0; i < values.length; i++)
            Expanded(
              child: Text(labels[i],
                  textAlign: TextAlign.center,
                  style: const TextStyle(fontSize: 7.5, color: Cozy.dimGray)),
            ),
        ]),
      ],
    );
  }
}
