import 'package:flutter/material.dart';

import '../core/cozy_palette.dart';

/// Raw 512-sample acoustic waveform (ADC counts 0–4095) — used live during
/// data collection to confirm a clean tap was captured.
class WaveformChart extends StatelessWidget {
  final List<double>? samples;
  final double height;
  const WaveformChart({super.key, required this.samples, this.height = 150});

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      painter: _WaveformPainter(samples),
      size: Size(double.infinity, height),
    );
  }
}

class _WaveformPainter extends CustomPainter {
  final List<double>? samples;
  _WaveformPainter(this.samples);

  @override
  void paint(Canvas canvas, Size size) {
    final gridPaint = Paint()
      ..color = Colors.white.withValues(alpha: 0.06)
      ..strokeWidth = 1;
    for (var i = 0; i <= 4; i++) {
      final y = size.height * i / 4;
      canvas.drawLine(Offset(0, y), Offset(size.width, y), gridPaint);
    }

    if (samples == null || samples!.isEmpty) {
      final tp = TextPainter(
        text: TextSpan(
            text: 'no tap yet — arm and tap when prompted',
            style: TextStyle(
                fontSize: 14, color: Colors.white.withValues(alpha: 0.3))),
        textDirection: TextDirection.ltr,
      )..layout();
      tp.paint(canvas,
          Offset((size.width - tp.width) / 2, (size.height - tp.height) / 2));
      return;
    }

    final s = samples!;
    var lo = 4095.0, hi = 0.0;
    for (final v in s) {
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    if (hi - lo < 1) {
      hi = lo + 1;
    }
    double y(double v) =>
        size.height - (v - lo) / (hi - lo) * (size.height - 14) - 4;

    final n = s.length;
    final pts = List<Offset>.generate(n,
        (i) => Offset(i / (n - 1) * size.width, y(s[i])));

    final fill = Path()..moveTo(0, size.height);
    for (final p in pts) {
      fill.lineTo(p.dx, p.dy);
    }
    fill.lineTo(size.width, size.height);
    canvas.drawPath(
        fill,
        Paint()
          ..shader = LinearGradient(
            begin: Alignment.topCenter,
            end: Alignment.bottomCenter,
            colors: [
              Cozy.matcha.withValues(alpha: 0.25),
              Cozy.matcha.withValues(alpha: 0.02),
            ],
          ).createShader(Rect.fromLTWH(0, 0, size.width, size.height)));

    final line = Path()..moveTo(pts.first.dx, pts.first.dy);
    for (final p in pts) {
      line.lineTo(p.dx, p.dy);
    }
    canvas.drawPath(
        line,
        Paint()
          ..color = Cozy.matcha
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1.4);

    // peak readout pill
    final peak = (hi - lo).round();
    final tp = TextPainter(
      text: TextSpan(
        text: 'peak Δ ${peak.round()}',
        style: const TextStyle(
            fontSize: 13, color: Cozy.matcha, fontWeight: FontWeight.bold),
      ),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(canvas, const Offset(8, 6));
  }

  @override
  bool shouldRepaint(covariant _WaveformPainter old) => old.samples != samples;
}
