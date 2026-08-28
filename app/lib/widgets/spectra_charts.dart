import 'package:flutter/material.dart';

/// Single-series plot payload (one ripeness class at a time).
class PlotSeries {
  final String label;
  final Color color;
  final List<double> values;
  const PlotSeries(this.label, this.color, this.values);
}

/// Live 8-bar hue-histogram chart — bars painted in their TRUE hue colour
/// (device window 20–120°, bins centred 26.25°…113.75°), like collectorrr.
class HueHistogramChart extends StatelessWidget {
  final PlotSeries? series;
  final double height;
  const HueHistogramChart({super.key, required this.series, this.height = 150});

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      painter: _HueBarsPainter(series),
      size: Size(double.infinity, height),
    );
  }
}

class _HueBarsPainter extends CustomPainter {
  final PlotSeries? series;
  _HueBarsPainter(this.series);

  static const winMin = 20.0, winMax = 120.0;
  static const binWidthDeg = (winMax - winMin) / 8; // 12.5°
  static const centres = [
    26.25, 38.75, 51.25, 63.75, 76.25, 88.75, 101.25, 113.75,
  ];

  double _x(double angleDeg, double w) => (angleDeg - winMin) / (winMax - winMin) * w;

  Color _binColor(double angle) =>
      HSVColor.fromAHSV(1.0, angle, 1.0, 1.0).toColor();

  @override
  void paint(Canvas canvas, Size size) {
    // horizontal gridlines (y = 0 … 1)
    final gridPaint = Paint()
      ..color = Colors.white.withValues(alpha: 0.07)
      ..strokeWidth = 1;
    for (var i = 0; i <= 4; i++) {
      final yy = size.height * (1 - i / 4);
      canvas.drawLine(Offset(0, yy), Offset(size.width, yy), gridPaint);
    }

    if (series == null || series!.values.length != 8) {
      _label(canvas, size, 'enable a class to see its hue signature');
      return;
    }
    final vals = series!.values;

    final paint = Paint()..style = PaintingStyle.fill;
    for (var b = 0; b < 8; b++) {
      final v = vals[b].clamp(0.0, 1.0);
      final cx = _x(centres[b], size.width);
      final halfW =
          _x(centres[b] + binWidthDeg * 0.46, size.width) - cx;
      final h = v * (size.height - 16);

      // faint full-height slot showing where the bin lives
      paint.color = _binColor(centres[b]).withValues(alpha: 0.13);
      canvas.drawRRect(
        RRect.fromRectAndCorners(
          Rect.fromLTWH(cx - halfW, 2, halfW * 2, size.height - 18),
          topLeft: const Radius.circular(3),
          topRight: const Radius.circular(3),
        ),
        paint,
      );

      // the value bar itself, in the bin's true hue colour
      paint.color = _binColor(centres[b]);
      canvas.drawRRect(
        RRect.fromRectAndCorners(
          Rect.fromLTWH(cx - halfW, size.height - 14 - h, halfW * 2, h),
          topLeft: const Radius.circular(3),
          topRight: const Radius.circular(3),
        ),
        paint,
      );
    }

    // angle labels under first / middle / last bins
    for (final b in const {0, 3, 7}) {
      _text(canvas, size, '${centres[b].toStringAsFixed(0)}°',
          _x(centres[b], size.width), size.height - 11, center: true);
    }
  }

  void _label(Canvas canvas, Size size, String msg) {
    final tp = TextPainter(
      text: TextSpan(
          text: msg,
          style:
              TextStyle(fontSize: 10, color: Colors.white.withValues(alpha: 0.3))),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(canvas,
        Offset((size.width - tp.width) / 2, (size.height - tp.height) / 2));
  }

  void _text(Canvas canvas, Size size, String msg, double x, double y,
      {bool center = false}) {
    final tp = TextPainter(
      text: TextSpan(
          text: msg,
          style:
              TextStyle(fontSize: 9, color: Colors.white.withValues(alpha: 0.35))),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(canvas, Offset(center ? x - tp.width / 2 : x, y));
  }

  @override
  bool shouldRepaint(covariant _HueBarsPainter old) => old.series != series;
}

/// Spectrum chart with two modes:
///   • 15 points (Rule Builder) → conditioned log-power at the firmware band
///     centres, drawn as a smooth spline with a dot on each centre and
///     frequency labels (150 Hz · 1.1 kHz · 2.1 kHz).
///   • dense (Data Collection live/preview) → the real full-resolution FFT
///     log-magnitude curve over 150 Hz–2.1 kHz, drawn as a smooth line with
///     no per-point dots.
class SpectrumChart extends StatelessWidget {
  final PlotSeries? series;
  final double height;
  const SpectrumChart({super.key, required this.series, this.height = 170});

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      painter: _SpectrumPainter(series),
      size: Size(double.infinity, height),
    );
  }
}

class _SpectrumPainter extends CustomPainter {
  final PlotSeries? series;
  _SpectrumPainter(this.series);

  static const centers = [
    150, 250, 350, 450, 550, 650, 750, 850, 950,
    1100, 1300, 1500, 1700, 1900, 2100,
  ];

  static const denseBand = [150, 1100, 2100]; // end labels for the dense mode

  /// Auto-scaled Y range that always includes 0 and pads symmetrically, so
  /// the same chart serves both the conditioned log-power (Rule Builder,
  /// ≈-10..0) and the dense log-magnitude (Data Collection, positive span)
  /// without a hardcoded window.
  (double, double) _range(List<double> v) {
    var mn = 0.0, mx = 0.0;
    for (final x in v) {
      final c = x.clamp(-100.0, 100.0);
      if (c < mn) mn = c;
      if (c > mx) mx = c;
    }
    final span = (mx - mn) == 0 ? 1.0 : (mx - mn);
    final pad = span * 0.15;
    return (mn - pad, mx + pad);
  }

  @override
  void paint(Canvas canvas, Size size) {
    final gridPaint = Paint()
      ..color = Colors.white.withValues(alpha: 0.07)
      ..strokeWidth = 1;
    for (var i = 0; i <= 4; i++) {
      final y = size.height * i / 4;
      canvas.drawLine(Offset(0, y), Offset(size.width, y), gridPaint);
    }

    final vals = series?.values;
    if (vals == null || vals.isEmpty) {
      final tp = TextPainter(
        text: TextSpan(
            text: 'enable a class to see its tap spectrum',
            style: TextStyle(
                fontSize: 10, color: Colors.white.withValues(alpha: 0.3))),
        textDirection: TextDirection.ltr,
      )..layout();
      tp.paint(canvas,
          Offset((size.width - tp.width) / 2, (size.height - tp.height) / 2));
      return;
    }

    final dense = vals.length != centers.length;
    final color = series!.color;
    final (lo, hi) = _range(vals);

    double y(double v) =>
        size.height - ((v - lo) / (hi - lo)) * (size.height - 18) - 4;

    // zero baseline (only when data straddles 0, e.g. mean-normalized shape)
    if (lo < 0 && hi > 0) {
      final y0 = y(0);
      canvas.drawLine(Offset(0, y0), Offset(size.width, y0),
          Paint()
            ..color = Colors.white.withValues(alpha: 0.14)
            ..strokeWidth = 1);
    }

    void drawCurve(List<Offset> pts, {bool round = false}) {
      final fill = Path()..moveTo(pts.first.dx, size.height);
      for (final p in pts) {
        fill.lineTo(p.dx, p.dy);
      }
      fill.lineTo(pts.last.dx, size.height);
      canvas.drawPath(
          fill,
          Paint()
            ..shader = LinearGradient(
              begin: Alignment.topCenter,
              end: Alignment.bottomCenter,
              colors: [
                color.withValues(alpha: 0.30),
                color.withValues(alpha: 0.02),
              ],
            ).createShader(Rect.fromLTWH(0, 0, size.width, size.height)));

      if (round) {
        // smooth spline through sparse control points
        final line = Path()..moveTo(pts.first.dx, pts.first.dy);
        for (var i = 0; i + 1 < pts.length; i++) {
          final mid = Offset((pts[i].dx + pts[i + 1].dx) / 2,
              (pts[i].dy + pts[i + 1].dy) / 2);
          line.quadraticBezierTo(pts[i].dx, pts[i].dy, mid.dx, mid.dy);
        }
        line.lineTo(pts.last.dx, pts.last.dy);
        canvas.drawPath(
            line,
            Paint()
              ..color = color
              ..style = PaintingStyle.stroke
              ..strokeWidth = 2.2
              ..strokeCap = StrokeCap.round);
      } else {
        // dense: straight polyline is faithful at this resolution
        final line = Path()..moveTo(pts.first.dx, pts.first.dy);
        for (var i = 1; i < pts.length; i++) {
          line.lineTo(pts[i].dx, pts[i].dy);
        }
        canvas.drawPath(
            line,
            Paint()
              ..color = color
              ..style = PaintingStyle.stroke
              ..strokeWidth = 2.2
              ..strokeCap = StrokeCap.round
              ..strokeJoin = StrokeJoin.round);
      }
    }

    if (dense) {
      // real full-resolution log-magnitude curve (150 Hz → 2.1 kHz)
      final pts = List<Offset>.generate(
          vals.length,
          (i) => Offset(i / (vals.length - 1) * size.width, y(vals[i])));
      drawCurve(pts);
      for (final f in denseBand) {
        final frac = (f - denseBand.first) /
            (denseBand.last - denseBand.first).toDouble();
        _text(canvas, size, _fmtHz(f), frac * size.width, size.height - 11,
            center: true);
      }
      return;
    }

    // conditioned log-power at the 15 firmware band centres (Rule Builder)
    final pts = List<Offset>.generate(
        centers.length,
        (i) =>
            Offset(i / (centers.length - 1) * size.width, y(vals[i])));
    drawCurve(pts, round: true);
    for (final p in pts) {
      canvas.drawCircle(p, 2.4, Paint()..color = color);
    }
    for (final b in const {0, 7, 14}) {
      _text(canvas, size, '${centers[b]} Hz', pts[b].dx, size.height - 11,
          center: true);
    }
  }

  static String _fmtHz(int hz) => hz >= 1000 ? '${hz ~/ 1000}.${(hz % 1000) ~/ 100} kHz' : '$hz Hz';

  void _text(Canvas canvas, Size size, String msg, double x, double y,
      {bool center = false}) {
    final tp = TextPainter(
      text: TextSpan(
          text: msg,
          style: TextStyle(
              fontSize: 9, color: Colors.white.withValues(alpha: 0.35))),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(canvas, Offset(center ? x - tp.width / 2 : x, y));
  }

  @override
  bool shouldRepaint(covariant _SpectrumPainter old) => old.series != series;
}
