import 'dart:convert' show base64Decode, base64Encode, utf8;
import 'dart:math' as math;
import 'dart:typed_data';

/// Dart port of datacollection/rules_engine.py — human knobs -> 616-byte
/// Fruit28D .bin using the exact firmware math.
class RulesModel {
  RulesModel._();

  // ── Device-exact constants ─────────────────────────────────────────
  static const hueWindowMin = 20.0, hueWindowMax = 120.0, hueBinCount = 8;
  static const hueBinWidth = (hueWindowMax - hueWindowMin) / hueBinCount;
  static final List<double> hueBinCentres = List.generate(
      hueBinCount, (i) => hueWindowMin + hueBinWidth * (i + 0.5));
  static const eightMaxVar = 0.875;
  static const volumeNeutralCm3 = 150.0;

  static const List<double> fftCenters = [
    150, 250, 350, 450, 550, 650, 750, 850, 950,
    1100, 1300, 1500, 1700, 1900, 2100,
  ];

  static const numClasses = 5;
  static const dims = 28;
  static const wireBytes = 32 + numClasses * dims * 4 + numClasses * 4 + 4;
  static const wireBytesLegacy = wireBytes - 4;

  /// CLASS_LABELS order — keep in sync with sci_28d.cpp / protocol.dart.
  static const classLabels = [
    'UNRIPE',
    'PERFECTLY_RIPE',
    'OVERRIPE',
    'ROTTEN_OR_HOLLOW',
    'ARTIFICIALLY_RIPENED',
  ];

  /// Human-facing knob limits.
  static const knobLimits = {
    'skin_hue_deg': (20.0, 120.0),
    'spread_deg': (5.0, 45.0),
    'firmness': (0.0, 1.0),
    'character': (0.0, 1.0),
  };

  /// Category presets straight from rules_engine.PRESETS.
  static const presets = <String, Map<String, double>>{
    'UNRIPE': {
      'skin_hue_deg': 102, 'spread_deg': 14, 'firmness': 0.05, 'character': 0.15,
    },
    'PERFECTLY_RIPE': {
      'skin_hue_deg': 38, 'spread_deg': 18, 'firmness': 0.80, 'character': 0.70,
    },
    'OVERRIPE': {
      'skin_hue_deg': 28, 'spread_deg': 34, 'firmness': 0.97, 'character': 0.95,
    },
    'ROTTEN_OR_HOLLOW': {
      'skin_hue_deg': 60, 'spread_deg': 42, 'firmness': 0.90, 'character': 0.88,
    },
    'ARTIFICIALLY_RIPENED': {
      'skin_hue_deg': 36, 'spread_deg': 10, 'firmness': 0.10, 'character': 0.25,
    },
  };

  // ── Knobs -> 28-D state ────────────────────────────────────────────

  static double _clamp(double v, double lo, double hi) =>
      v < lo ? lo : (v > hi ? hi : v);

  static List<double> hueHistogram(double skinHueDeg, double spreadDeg) {
    final bump = hueBinCentres
        .map((c) => math.exp(-0.5 * math.pow((c - skinHueDeg) / spreadDeg, 2)))
        .toList();
    final total = bump.fold(0.0, (a, b) => a + b);
    return total > 0
        ? bump.map((v) => v / total).toList()
        : List.filled(8, 0.125);
  }

  static double chromaticDispersion(List<double> hist) {
    final mean = hist.fold(0.0, (a, b) => a + b) / hist.length;
    var vari = 0.0;
    for (final v in hist) {
      vari += (v - mean) * (v - mean);
    }
    vari /= hist.length;
    return _clamp(1.0 - vari / eightMaxVar, 0.0, 1.0);
  }

  /// dict(skin_hue_deg, spread_deg, firmness, character) -> 28 doubles.
  static List<double> stateFromKnobs(Map<String, double> knobs) {
    final h =
        _clamp(knobs['skin_hue_deg'] ?? 60, 20.0, 120.0);
    final s = _clamp(knobs['spread_deg'] ?? 15, 5.0, 45.0);
    final firm = _clamp(knobs['firmness'] ?? 0.5, 0.0, 1.0);
    final char_ = _clamp(knobs['character'] ?? 0.5, 0.0, 1.0);

    final state = List<double>.filled(dims, 0);
    final hist = hueHistogram(h, s);
    state.setRange(0, 8, hist);
    state[8] = chromaticDispersion(hist);

    final vol23 = math.pow(volumeNeutralCm3, 2.0 / 3.0).toDouble();
    state[9] = _clamp((vol23 - 10.0) / (300.0 - 10.0), 0.0, 1.0);

    final fCentre = 1000.0 * math.pow(0.3, firm).toDouble();
    final sigmaLn = 0.35 + 0.85 * char_;
    const peak = -0.9, floor = -7.6;
    for (var i = 0; i < fftCenters.length; i++) {
      final lnRatio = math.log(fftCenters[i] / fCentre);
      final prof =
          floor + (peak - floor) * math.exp(-0.5 * math.pow(lnRatio / sigmaLn, 2));
      state[10 + i] = math.max(prof, -10.0);
    }

    state[25] = 0.30 + 0.55 * char_;
    return state;
  }

  // ── Nearest-proto bridge ───────────────────────────────────────────

  /// Knobs -> prototypes -> bridge. Thin wrapper; real work in
  /// [buildFromPrototypes].
  static ({List<List<double>> w, List<double> b, int mask, double temp})
      buildRulesModel(
      Map<String, List<double>> statesByLabel, List<String> enabledLabels,
      {double tempFactor = 1.0}) {
    return buildFromPrototypes(statesByLabel, enabledLabels,
        tempFactor: tempFactor);
  }

  /// Prototype vectors + enabled labels -> packed model parts.
  ///
  /// Per-dim ranges use a SCALE-AWARE FLOOR:
  ///   r_d = max(range_d, 0.10, 0.05*max_c|p_c,d|)
  /// Without it, two classes that nearly agree on a dim (diff 0.02) get
  /// 1/r^2 amplified into ~2000-weight dims that drown the real signal
  /// (recorded mango: 6/13 -> 11/13 with this floor).
  ///
  /// temp is AUTO-CALIBRATED: worst-case winner-vs-runner-up margin at the
  /// prototypes maps to ~90% confidence; [tempFactor] multiplies that.
  static ({List<List<double>> w, List<double> b, int mask, double temp})
      buildFromPrototypes(
      Map<String, List<double>> protos, List<String> enabledLabels,
      {double tempFactor = 1.0}) {
    final labels = classLabels.where((l) => protos.containsKey(l)).toList();
    assert(labels.isNotEmpty, 'no categories given');
    assert(enabledLabels.isNotEmpty, 'no categories enabled');

    final ranges = List<double>.filled(dims, 1.0);
    for (var d = 0; d < dims; d++) {
      var mn = double.infinity, mx = double.negativeInfinity, ma = 0.0;
      for (final l in labels) {
        mn = math.min(mn, protos[l]![d]);
        mx = math.max(mx, protos[l]![d]);
        ma = math.max(ma, protos[l]![d].abs());
      }
      ranges[d] = math.max((mx - mn), math.max(0.10, 0.05 * ma));
    }

    final w = List.generate(numClasses, (_) => List<double>.filled(dims, 0));
    final b = List<double>.filled(numClasses, 0);
    for (final lab in labels) {
      final c = classLabels.indexOf(lab);
      final p = protos[lab]!;
      var sumSq = 0.0;
      for (var d = 0; d < dims; d++) {
        final r2 = ranges[d] * ranges[d];
        w[c][d] = 2.0 * p[d] / r2;
        sumSq += p[d] * p[d] / r2;
      }
      b[c] = -sumSq;
    }

    var mask = 0;
    for (final lab in enabledLabels) {
      final idx = classLabels.indexOf(lab);
      if (idx >= 0) mask |= 1 << idx;
    }

    // auto-temp: worst-case prototype margin -> ~90% target
    double worstMargin = double.infinity;
    for (final lab in labels) {
      final scores = classify(protos[lab]!, w, b, mask);
      final own = classLabels.indexOf(lab);
      double bestOther = double.negativeInfinity;
      for (var c = 0; c < numClasses; c++) {
        if (c == own || (((mask >> c) & 1) == 0)) continue;
        if (scores[c] > bestOther) bestOther = scores[c];
      }
      if (bestOther == double.negativeInfinity) continue;
      final margin = scores[own] - bestOther;
      if (margin < worstMargin) worstMargin = margin;
    }
    // ln(9) ~ 2.197 -> 90/10 odds at the prototype itself; real sensor
    // noise then pulls confidence below this - honest headroom.
    final autoTemp = (worstMargin == double.infinity || worstMargin <= 0)
        ? 2.0
        : (worstMargin / 2.1972);
    final effTemp = (autoTemp * tempFactor).clamp(0.05, 500.0);

    for (final lab in labels) {
      final c = classLabels.indexOf(lab);
      for (var d = 0; d < dims; d++) {
        w[c][d] /= effTemp;
      }
    }
    for (var c = 0; c < numClasses; c++) {
      b[c] /= effTemp;
    }
    return (w: w, b: b, mask: mask, temp: effTemp);
  }
  /// Mirror evaluate_fruit_single(): disabled classes score -100000.
  static List<double> classify(
      List<double> state, List<List<double>> w, List<double> b, int mask) {
    final scores = List<double>.generate(numClasses, (c) {
      if (((mask >> c) & 1) == 0) return -100000.0;
      var s = b[c];
      for (var d = 0; d < dims; d++) {
        s += w[c][d] * state[d];
      }
      return s;
    });
    return scores;
  }

  /// Perturbed prototypes must classify as themselves (rules_engine.selftest).
  static bool selftest(Map<String, List<double>> states, List<List<double>> w,
      List<double> b, int mask,
      {int trialsPerClass = 40, int seed = 0}) {
    final rng = math.Random(seed);
    final labels = states.keys.toList();
    for (final lab in labels) {
      final base = states[lab]!;
      for (var t = 0; t <= trialsPerClass; t++) {
        final List<double> x;
        if (t == 0) {
          x = base;
        } else {
          x = List<double>.generate(dims, (d) {
            final perturb = rng.nextDouble() < 0.3;
            return base[d] +
                (perturb ? _gauss(rng) * 0.03 : 0.0);
          });
        }
        final scores = classify(x, w, b, mask);
        var best = 0;
        for (var c = 1; c < numClasses; c++) {
          if (scores[c] > scores[best]) best = c;
        }
        // nearest range-normalised prototype among provided labels
        final ranges = List<double>.filled(dims, 1.0);
        for (var d = 0; d < dims; d++) {
          var mn = double.infinity, mx = double.negativeInfinity;
          for (final l in labels) {
            mn = math.min(mn, states[l]![d]);
            mx = math.max(mx, states[l]![d]);
          }
          ranges[d] = (mx - mn) == 0 ? 1.0 : (mx - mn);
        }
        var refIdx = 0;
        var refDist = double.infinity;
        for (var i = 0; i < labels.length; i++) {
          var dist = 0.0;
          for (var d = 0; d < dims; d++) {
            final diff = (x[d] - states[labels[i]]![d]) / ranges[d];
            dist += diff * diff;
          }
          if (dist < refDist) {
            refDist = dist;
            refIdx = i;
          }
        }
        if (classLabels[best] != labels[refIdx]) return false;
      }
    }
    return true;
  }

  static double _gauss(math.Random rng) {
    // Box-Muller
    final u1 = math.max(rng.nextDouble(), 1e-12);
    final u2 = rng.nextDouble();
    return math.sqrt(-2 * math.log(u1)) * math.cos(2 * math.pi * u2);
  }

  // ── Measured prototypes (mango, dataset `50%-complete`) ────────────
  // Class means of the 28-D real feature vectors (13 taps: 6 UNRIPE,
  // 7 PERFECTLY_RIPE). Extraction mirrors firmware conditioning.
  // LOO validation: 11/13, median confidence 89% (see
  // notes/rules-bridge-calibration.md).
  static const String measuredSourceName =
      'mango-recording-2026-08 (50%-complete branch)';

  static const List<double> measuredUNRIPE = [
    0.5421, 0.0194, 0.4288, 0.0000,
    0.0016, 0.0000, 0.0000, 0.0081,
    0.9495, 0.0629, 0.6440, 1.5552,
    3.1562, 6.1648, 8.6951, 12.5689,
    15.0916, 17.7073, 22.0382, 28.8408,
    39.5672, 51.8683, 71.0340, 79.1435,
    102.5381, 0.2000, 0.0000, 0.0000,
  ];

  static const List<double> measuredPERFECTLYRIPE = [
    0.4745, 0.0609, 0.4082, 0.0038,
    0.0046, 0.0171, 0.0159, 0.0150,
    0.9612, 0.0629, 0.6559, 1.4639,
    3.4818, 5.8933, 8.2917, 10.6937,
    14.3089, 16.6865, 20.1426, 24.8715,
    36.8619, 44.2746, 57.3268, 69.1246,
    87.3102, 0.2000, 0.0000, 0.0000,
  ];

  static const Map<String, List<double>> measuredPrototypes = {
    'UNRIPE': measuredUNRIPE,
    'PERFECTLY_RIPE': measuredPERFECTLYRIPE,
  };

  // ── Packing ────────────────────────────────────────────────────────

  /// name + W + b + mask -> 616-byte blob ready for BLE upload / NVS.
  static Uint8List packBinary(String name, List<List<double>> w,
      List<double> b, int mask) {
    final blob = BytesBuilder();

    final nameBytes = Uint8List(32); // byte 31 must stay clear
    final rawName = utf8.encode(name);
    assert(rawName.length <= 31, 'model name too long');
    nameBytes.setRange(0, math.min(rawName.length, 31), rawName);
    blob.add(nameBytes);

    final bd = ByteData(w.length * dims * 4 + b.length * 4);
    var off = 0;
    for (final row in w) {
      for (final v in row) {
        bd.setFloat32(off, v, Endian.little);
        off += 4;
      }
    }
    for (final v in b) {
      bd.setFloat32(off, v, Endian.little);
      off += 4;
    }
    blob.add(bd.buffer.asUint8List());

    blob.add(Uint8List.fromList([mask & 0xFF, 0, 0, 0]));
    final out = blob.toBytes();
    assert(out.length == wireBytes);
    return out;
  }

  // ── .bin import / export helpers ───────────────────────────────────

  static String toBase64(Uint8List bin) => base64Encode(bin);

  static Uint8List? tryParseBinText(String text) {
    final cleaned = text.replaceAll(RegExp(r'\s+'), '');
    if (cleaned.isEmpty) return null;
    Uint8List? bytes;
    if (cleaned.length % 4 == 0 && !cleaned.contains(RegExp(r'[^A-Za-z0-9+/=]'))) {
      try {
        bytes = base64Decode(cleaned);
      } catch (_) {}
    }
    bytes ??= () {
      if (!cleaned.contains(RegExp(r'^[0-9a-fA-F]+$'))) return null;
      if (cleaned.length.isOdd) return null;
      final out = Uint8List(cleaned.length ~/ 2);
      for (var i = 0; i < out.length; i++) {
        out[i] = int.parse(cleaned.substring(i * 2, i * 2 + 2), radix: 16);
      }
      return out;
    }();
    if (bytes == null) return null;
    if (bytes.length != wireBytes && bytes.length != wireBytesLegacy) {
      return null;
    }
    return bytes;
  }

  /// Fruit name stored at offset 0 of a model blob.
  static String binName(Uint8List bin) {
    final end = bin.indexWhere((b) => b == 0, 0);
    return utf8.decode(bin.sublist(0, end < 0 ? 31 : end), allowMalformed: true);
  }

  /// HSV hue degrees -> UI swatch color (S=V=1).
  static int hueSwatch(double hDeg) {
    final h = (hDeg % 360) / 60.0;
    final i = h.floor() % 6;
    final f = h - h.floorToDouble();
    // standard HSV->RGB sector table
    final sectors = [
      [1.0, f, 0.0],
      [1.0 - f, 1.0, 0.0],
      [0.0, 1.0, f],
      [0.0, 1.0 - f, 1.0],
      [f, 0.0, 1.0],
      [1.0, 0.0, 1.0 - f],
    ];
    final rgb = sectors[i];
    final r = (rgb[0] * 255).round();
    final g = (rgb[1] * 255).round();
    final b = (rgb[2] * 255).round();
    return (0xFF << 24) | (r << 16) | (g << 8) | b;
  }
}

// ── .bin editing helpers (firmware has no model read-back, so edits work
//    on bins we still hold in the app vault) ─────────────────────────
class BinEdit {
  BinEdit._();

  /// active_class_mask byte at offset 612 (616-byte wire format).
  static int maskOf(Uint8List bin) =>
      bin.length >= RulesModel.wireBytes ? bin[612] : 0x1F;

  static Uint8List withMask(Uint8List bin, int mask) {
    if (bin.length < RulesModel.wireBytes) return bin; // legacy
    final out = Uint8List.fromList(bin);
    out[612] = mask & 0xFF;
    return out;
  }

  static Uint8List withName(Uint8List bin, String name) {
    final out = Uint8List.fromList(bin);
    final raw = utf8.encode(name);
    for (var i = 0; i < 31; i++) {
      out[i] = i < raw.length ? raw[i] : 0;
    }
    return out;
  }

  /// factor > 1 softens boundaries (W,b ÷ factor); < 1 sharpens.
  static Uint8List scaledBy(Uint8List bin, double factor) {
    final out = Uint8List.fromList(bin);
    final bd = ByteData.sublistView(out);
    const wOff = 32;
    final wBytes = RulesModel.numClasses * RulesModel.dims * 4;
    final bOff = wOff + wBytes;
    for (var off = wOff; off < bOff + RulesModel.numClasses * 4; off += 4) {
      final v = bd.getFloat32(off, Endian.little);
      bd.setFloat32(off, v / factor, Endian.little);
    }
    return out;
  }
}
