import 'dart:convert' show base64Decode, base64Encode, utf8;
import 'dart:math' as math;
import 'dart:typed_data';

/// 32-D Fruit Profile Engine (app-side).
///
/// Data model + archetype library + the deterministic 6-step Rule Compiler that
/// produces the exact 852-byte `ManifoldModel32D` wire blob the firmware
/// (esp/main/sci_32d.cpp, bt_manager.cpp, fruit_store.cpp) understands:
///   name[32] | mask@32 | gStart@33 | gEnd@34 | flags@35 |
///   veto@36,minVol@40,maxVol@44,f0@48,damp@52,reserved@56-63 |
///   feature_weights@64-191 | prototypes[5][32]@192 | biases@832-851
///
/// Block layout (must match the device's assemble_state_32d):
///   dims 0..9    optics & mass (8 hue bins + dispersion + volume^(2/3))
///   dims 10..25  acoustic image (4x4 contrast-normed STFT grid)
///   dims 26..31  bio-moments (centroid, tail, harmonic, abbott, entropy,
///                damping)
///
/// v3 prototypes are RAW class MEANS (block normalization removed so acoustic
/// amplitude survives); the firmware scores a Diagonal-Gaussian (Mahalanobis)
/// with `feature_weights` as pooled inverse-variance:
///   score[c] = bias[c] - 0.5 * sum_d invVar[d]*(state[d]-mean[c][d])^2
/// exactly like the validated `FruitProfile32D.fit_prototypes`.
class RulesModel32D {
  RulesModel32D._();

  // ── Constants (must match sci_32d.h / test_fruit_profile_engine.py) ─
  static const dims = 32;
  static const numClasses = 5;
  static const wireBytes = 852;
  static const wireBytesLegacy = 852 - 4;

  static const greenBinsStart = 5;
  static const greenBinsEnd = 8;
  static const greenVetoDefault = 0.25;
  static const minVolumeCm3 = 10.0;
  static const maxVolumeCm3 = 600.0;
  static const expectedF0Hz = 180.0;

  /// CLASS_LABELS order — keep in sync with sci_32d.cpp / protocol.dart.
  static const classLabels = [
    'UNRIPE',
    'PERFECTLY_RIPE',
    'OVERRIPE',
    'ROTTEN_OR_HOLLOW',
    'ARTIFICIALLY_RIPENED',
  ];

  // ── Wire offsets ───────────────────────────────────────────────────
  static const offsetMask = 32;
  static const offsetGreenStart = 33;
  static const offsetGreenEnd = 34;
  static const offsetFlags = 35;
  static const offsetVeto = 36;
  static const offsetMinVol = 40;
  static const offsetMaxVol = 44;
  static const offsetF0 = 48;
  static const offsetDamp = 52;
  static const offsetReserved = 56;
  static const offsetFeatureWeights = 64;
  static const offsetPrototypes = 192;
  static const offsetBiases = 832;

  // ── Knob delta scales (spec Step 2) ────────────────────────────────
  static const stiffnessScale = 0.30;
  static const dampingScale = 0.35;
  static const tailScale = 0.25;
  static const hashScaleHarmonic = 0.30;
  static const hashScaleEntropy = 0.20;
  static const hashScaleBand3 = 0.25;

  static double _clamp(double v, double lo, double hi) =>
      v < lo ? lo : (v > hi ? hi : v);

  // ── Named & positional prototype access ────────────────────────────
  static int indexOf(String label) {
    final i = classLabels.indexOf(label);
    return i < 0 ? 0 : i;
  }

  // ── Step 2: hue gaussian (8-bin histogram for dims 0..7) ───────────
  /// Bin centres 32.5..107.5 in 12.5° steps over [20,120] (25° per bin;
  /// centre = lo + width*(i+0.5)). Mirrors the device/validated engine's
  /// hue rendering used to synthesize a skin signature.
  static List<double> hueGaussian(double hueTargetDeg, double hueSpreadDeg) {
    const lo = 20.0, hi = 120.0, bins = 8;
    const width = (hi - lo) / bins;
    double centre(int i) => lo + width * (i + 0.5);
    final spread = _clamp(hueSpreadDeg, 1.0, 60.0);
    final bump = List<double>.generate(
        bins, (i) => math.exp(-0.5 *
            math.pow((centre(i) - hueTargetDeg) / spread, 2).toDouble()));
    final total = bump.fold(0.0, (a, b) => a + b);
    if (total <= 0) return List.filled(8, 1.0 / 8);
    return bump.map((v) => _clamp(v / total, 0.0, 1.0)).toList();
  }

  /// Weighted-mean hue (deg) of an 8-bin hue histogram (dims 0..7), i.e. the
  /// "natural" skin colour a source encodes. Used by the UI to show an
  /// absolute hue centre that defaults to the source's intrinsic colour.
  static double hueFromBins(List<double> bins, {double fallback = 45.0}) {
    const lo = 20.0, hi = 120.0, n = 8;
    const width = (hi - lo) / n;
    double centre(int i) => lo + width * (i + 0.5);
    var total = 0.0, wm = 0.0;
    for (var i = 0; i < n; i++) {
      final v = (i < bins.length && bins[i].isFinite) ? bins[i] : 0.0;
      total += v;
      wm += v * centre(i);
    }
    return total > 1e-9 ? wm / total : fallback;
  }

  /// Intrinsic hue (deg) of a category source used as the offset origin for
  /// the hue knob: archetype -> its declared naturalHueDeg; measured ->
  /// weighted-mean of the measured centroid's dims 0..7.
  static double naturalHueFor(CategorySourceType sourceType,
      String? archetypePresetId, List<double>? measuredBase) {
    if (sourceType == CategorySourceType.measured && measuredBase != null) {
      return hueFromBins(measuredBase);
    }
    return ArchetypeLibrary.byId(archetypePresetId)?.naturalHueDeg ?? 45.0;
  }

  // ── Step 2: apply slider deltas to a base vector ───────────────────
  /// Applies the flesh-dynamics deltas only (keeps existing optics dims 0..9).
  /// Used for measured sources, where dims 0..9 hold real ms_captured optics.
  static List<double> applyFleshDeltas(List<double> base, CategoryKnobs knobs) {
    final z = List<double>.from(base);
    z[29] += knobs.stiffnessDelta * stiffnessScale;
    z[31] += knobs.dampingDelta * dampingScale;
    z[27] += knobs.resonanceTailDelta * tailScale;
    z[28] += knobs.highToneHashDelta * hashScaleHarmonic;
    z[30] += knobs.highToneHashDelta * hashScaleEntropy;
    for (final d in const [13, 17, 21, 25]) {
      z[d] += knobs.highToneHashDelta * hashScaleBand3;
    }
    return z;
  }

  /// Mirrors the spec's delta table. Replaces optics dims 0..9 with the
  /// synthesized gaussian hue (used for archetype sources). Operates on the
  /// RAW base state (block-normalization happens afterwards in Step 3).
  static List<double> applyDeltas(
      List<double> base, CategoryKnobs knobs, List<double> hueHist) {
    final z = List<double>.from(base);

    // Optics: replace hue dims 0..7 with the gaussian + dispersion.
    z.setRange(0, 8, hueHist);
    // Chromatic dispersion (device formula 1 - sum((h-1/8)^2)/0.875).
    var varSum = 0.0;
    for (var i = 0; i < 8; i++) {
      final d = hueHist[i] - 1.0 / 8.0;
      varSum += d * d;
    }
    z[8] = _clamp(1.0 - varSum / 0.875, 0.0, 1.0);
    // Volume^(2/3) norm for dim 9 (spec keeps at base).

    // Flesh dynamics.
    z[29] += knobs.stiffnessDelta * stiffnessScale; // Abbott stiffness
    z[31] += knobs.dampingDelta * dampingScale; // dynamic damping
    z[27] += knobs.resonanceTailDelta * tailScale; // tail energy
    z[28] += knobs.highToneHashDelta * hashScaleHarmonic; // harmonic absorption
    z[30] += knobs.highToneHashDelta * hashScaleEntropy; // spectral entropy

    // Band 3 (850-1400 Hz) cells of the 4x4 image: dims 13,17,21,25.
    for (final d in const [13, 17, 21, 25]) {
      z[d] += knobs.highToneHashDelta * hashScaleBand3;
    }

    return z;
  }

  // ── Step 3: block L2 normalization ─────────────────────────────────
  static void blockL2(List<double> s, int off, int len) {
    var n2 = 0.0;
    for (var i = 0; i < len; i++) {
      n2 += s[off + i] * s[off + i];
    }
    final inv = 1.0 / (math.sqrt(n2) + 1e-6);
    for (var i = 0; i < len; i++) {
      s[off + i] *= inv;
    }
  }

  // ── Step 4: apply global block weights -> unit prototype ───────────
  /// Weights the three blocks by [wOptics]/[wImage]/[wMoments] then
  /// unit-normalizes the full 32-D vector to a cosine prototype.
  static List<double> weightedPrototype(
      List<double> blockNormed, double wOptics, double wImage, double wMoments) {
    final z = List<double>.from(blockNormed);
    for (var i = 0; i < 10; i++) {
      z[i] *= wOptics;
    }
    for (var i = 10; i < 26; i++) {
      z[i] *= wImage;
    }
    for (var i = 26; i < 32; i++) {
      z[i] *= wMoments;
    }
    final n2 = z.fold(0.0, (a, v) => a + v * v);
    final inv = 1.0 / (math.sqrt(n2) + 1e-6);
    for (var i = 0; i < z.length; i++) {
      z[i] *= inv;
    }
    return z;
  }

  // ── Inference mirror (matches evaluate_scores) ─────────────────────
  /// v3 Diagonal-Gaussian score:
  ///   score[c] = bias[c] - 0.5 * sum_d invVar[d] * (state[d]-mean[c][d])^2
  /// [means] are the per-class raw-state means; [invVar] is the pooled
  /// per-feature inverse-variance carried in feature_weights. Disabled classes
  /// are pushed to -100000 exactly like the firmware.
  static List<double> rawScores(
      List<double> state, List<List<double>> means, List<double> biases,
      List<double> invVar, int mask) {
    final scores = List<double>.generate(numClasses, (c) {
      if (((mask >> c) & 1) == 0) return -100000.0;
      var d2 = 0.0;
      for (var d = 0; d < dims; d++) {
        final diff = state[d] - means[c][d];
        d2 += invVar[d] * diff * diff;
      }
      return biases[c] - 0.5 * d2;
    });

    // Green veto (bins 5..8).
    var greenMass = 0.0;
    for (var i = greenBinsStart; i < greenBinsEnd; i++) {
      greenMass += state[i];
    }
    if (greenMass > greenVetoDefault) {
      // Veto ripe + artificially ripened, matching the firmware.
      scores[1] = -999.0;
      scores[4] = -999.0;
    }
    return scores;
  }

  static List<double> softmax(List<double> raw) {
    var max = raw[0];
    for (final v in raw) {
      if (v > max) max = v;
    }
    var sum = 0.0;
    final e = List<double>.generate(raw.length, (i) => math.exp(raw[i] - max));
    for (final v in e) {
      sum += v;
    }
    if (sum < 1e-15) sum = 1e-15;
    return e.map((v) => v / sum).toList();
  }

  static int argmax(List<double> v) {
    var best = 0;
    for (var i = 1; i < v.length; i++) {
      if (v[i] > v[best]) best = i;
    }
    return best;
  }

  /// Mirror of evaluate_fruit_single_32d: returns winner index + probs.
  static ({int winner, List<double> probs}) classify32d(
      List<double> state,
      List<List<double>> means, //
      List<double> biases,
      List<double> invVar,
      int mask) {
    final raw = rawScores(state, means, biases, invVar, mask);
    final probs = softmax(raw);
    return (winner: argmax(probs), probs: probs);
  }

  // ── Separation matrix (UI model health) ────────────────────────────
  /// Cosine similarity of each pair of prototypes -> [n][n] matrix. v3 stores
  /// RAW class means (not unit vectors), so each pair is explicitly normalized
  /// by its L2 norms — a plain dot product of non-unit means can exceed 1 and
  /// is meaningless as a separation metric.
  static List<List<double>> separationMatrix(List<List<double>> protos) {
    final n = protos.length;
    final m = List.generate(n, (_) => List<double>.filled(n, 0.0));
    for (var i = 0; i < n; i++) {
      m[i][i] = 1.0;
      for (var j = i + 1; j < n; j++) {
        var dot = 0.0, na = 0.0, nb = 0.0;
        for (var d = 0; d < dims; d++) {
          dot += protos[i][d] * protos[j][d];
          na += protos[i][d] * protos[i][d];
          nb += protos[j][d] * protos[j][d];
        }
        final denom = math.sqrt(na) * math.sqrt(nb);
        final cos = denom > 1e-12 ? (dot / denom) : 0.0;
        m[i][j] = cos;
        m[j][i] = cos;
      }
    }
    return m;
  }

  /// Smallest cosine separator among active classes = model health metric.
  static double minMargin(List<List<double>> protos, List<String> active) {
    var min = 1.0;
    for (var i = 0; i < active.length; i++) {
      for (var j = i + 1; j < active.length; j++) {
        final a = indexOf(active[i]);
        final b = indexOf(active[j]);
        var dot = 0.0, na = 0.0, nb = 0.0;
        for (var d = 0; d < dims; d++) {
          dot += protos[a][d] * protos[b][d];
          na += protos[a][d] * protos[a][d];
          nb += protos[b][d] * protos[b][d];
        }
        final denom = math.sqrt(na) * math.sqrt(nb);
        final cos = denom > 1e-12 ? (dot / denom) : 1.0;
        if (cos < min) min = cos;
      }
    }
    return min;
  }

  // ── Selftest: perturbed class means classify as themselves ─────────
  static bool selftest32d(
      List<List<double>> means, //
      List<double> biases,
      List<double> invVar,
      int mask,
      List<String> active,
      {int trials = 20, int seed = 1}) {
    final rng = math.Random(seed);
    for (final label in active) {
      final c = indexOf(label);
      final base = means[c];
      for (var t = 0; t <= trials; t++) {
        final List<double> x;
        if (t == 0) {
          x = base;
        } else {
          x = List<double>.generate(dims,
              (d) => base[d] + (rng.nextDouble() < 0.3 ? _gauss(rng) * 0.03 : 0.0));
        }
        final r = classify32d(x, means, biases, invVar, mask);
        if (r.winner != c) return false;
      }
    }
    return true;
  }

  static double _gauss(math.Random rng) {
    final u1 = math.max(rng.nextDouble(), 1e-12);
    final u2 = rng.nextDouble();
    return math.sqrt(-2 * math.log(u1)) * math.cos(2 * math.pi * u2);
  }

  // ── Packing (852-byte wire, exact validated layout) ────────────────
  /// [name] (<=31), [means]/[protos] 5x32, [biases], [mask], [veto], plus the
  /// embedded profile scalars -> 852-byte blob.
  ///
  /// v3: [featureWeights] (optional, defaults to ones(32)) carries the pooled
  /// per-feature inverse-variance used by the Diagonal-Gaussian scorer.
  static Uint8List packBinary({
    required String name,
    required List<List<double>> protos,
    required List<double> biases,
    required int mask,
    List<double>? featureWeights,
    double greenVeto = greenVetoDefault,
    double minVol = minVolumeCm3,
    double maxVol = maxVolumeCm3,
    double f0 = expectedF0Hz,
  }) {
    final out = Uint8List(wireBytes);
    final bd = ByteData.sublistView(out);

    // name[32]
    final nameBytes = name.codeUnits.take(31).toList();
    for (var i = 0; i < nameBytes.length && i < 31; i++) {
      out[i] = nameBytes[i] & 0xFF;
    }
    out[31] = 0; // clear last byte

    out[offsetMask] = mask & 0xFF;
    out[offsetGreenStart] = greenBinsStart;
    out[offsetGreenEnd] = greenBinsEnd;
    out[offsetFlags] = 0;
    bd.setFloat32(offsetVeto, greenVeto, Endian.little);
    bd.setFloat32(offsetMinVol, minVol, Endian.little);
    bd.setFloat32(offsetMaxVol, maxVol, Endian.little);
    bd.setFloat32(offsetF0, f0, Endian.little);
    bd.setFloat32(offsetDamp, 1.0, Endian.little);
    // reserved[8] stays 0.

    // feature_weights = pooled inverse-variance (v3), default ones(32).
    for (var d = 0; d < 32; d++) {
      final v = (featureWeights != null && d < featureWeights.length)
          ? featureWeights[d]
          : 1.0;
      bd.setFloat32(offsetFeatureWeights + d * 4, v.isFinite ? v : 1.0,
          Endian.little);
    }

    // prototypes[5][32] = per-class means (v3)
    for (var c = 0; c < numClasses; c++) {
      for (var d = 0; d < dims; d++) {
        bd.setFloat32(
            offsetPrototypes + (c * dims + d) * 4,
            protos[c][d],
            Endian.little);
      }
    }

    // biases[5]
    for (var c = 0; c < numClasses; c++) {
      bd.setFloat32(offsetBiases + c * 4, biases[c], Endian.little);
    }

    assert(out.length == wireBytes);
    return out;
  }

  // ── .bin import / export / edit helpers ───────────────────────────

  static String toBase64(Uint8List bin) => base64Encode(bin);

  /// Base64 or hex of an 852-byte (or legacy 848-byte) blob -> bytes.
  static Uint8List? tryParseBinText(String text) {
    final cleaned = text.replaceAll(RegExp(r'\s+'), '');
    if (cleaned.isEmpty) return null;
    Uint8List? bytes;
    // A pure hex string (even char count, hex-only) must be decoded as hex,
    // even though it also looks like a valid base64 alphabet.
    final isHex = cleaned.length.isEven &&
        !cleaned.contains(RegExp(r'[^0-9a-fA-F]'));
    if (isHex) {
      final out = Uint8List(cleaned.length ~/ 2);
      for (var i = 0; i < out.length; i++) {
        out[i] = int.parse(cleaned.substring(i * 2, i * 2 + 2), radix: 16);
      }
      bytes = out;
    } else if (cleaned.length % 4 == 0 &&
        !cleaned.contains(RegExp(r'[^A-Za-z0-9+/=]'))) {
      try {
        bytes = base64Decode(cleaned);
      } catch (_) {}
    }
    if (bytes == null) return null;
    if (bytes.length != wireBytes && bytes.length != wireBytesLegacy) {
      return null;
    }
    return bytes;
  }

  /// Fruit name stored at bytes 0..31 of the blob.
  static String binName(Uint8List bin) {
    final end = bin.indexWhere((b) => b == 0, 0);
    return utf8.decode(
        bin.sublist(0, end < 0 ? 31 : end), allowMalformed: true);
  }

  /// Active-class mask stored at offset 32 (852-byte wire format).
  static int maskOf(Uint8List bin) =>
      bin.length >= wireBytes ? bin[offsetMask] : 0x1F;

  static Uint8List withMask(Uint8List bin, int mask) {
    if (bin.length < wireBytes) return bin; // legacy
    final out = Uint8List.fromList(bin);
    out[offsetMask] = mask & 0xFF;
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
}

// ── Category source type ─────────────────────────────────────────────
enum CategorySourceType { measured, archetype }

/// The 6 intuitive physical knobs (offsets applied to the base vector).
class CategoryKnobs {
  /// Skin-hue offset in degrees. Offset from the source's NATURAL hue: the
  /// compiler renders a synthesised gaussian skin signature only when this is
  /// non-zero. At 0 the source keeps its own optics (measured centroid or the
  /// archetype's intrinsic skin), so categories stay physically separated.
  double hueDeltaDeg;
  double hueSpread; // 0..1 (normalized; mapped to 1..60 deg)
  double stiffnessDelta; // -1..1 (shift Abbott & band 0->1)
  double dampingDelta; // -1..1 (shift decay slope & centroid)
  double resonanceTailDelta; // -1..1 (shift tail energy)
  double highToneHashDelta; // -1..1 (shift 850-1400Hz hash & entropy)

  CategoryKnobs({
    this.hueDeltaDeg = 0.0,
    this.hueSpread = 0.35,
    this.stiffnessDelta = 0.0,
    this.dampingDelta = 0.0,
    this.resonanceTailDelta = 0.0,
    this.highToneHashDelta = 0.0,
  });

  CategoryKnobs copy() => CategoryKnobs(
        hueDeltaDeg: hueDeltaDeg,
        hueSpread: hueSpread,
        stiffnessDelta: stiffnessDelta,
        dampingDelta: dampingDelta,
        resonanceTailDelta: resonanceTailDelta,
        highToneHashDelta: highToneHashDelta,
      );
}

/// One of up-to-5 slots in a FruitProfile.
class CategoryRule {
  final int id; // 0..4 (fixed slot)
  String name; // e.g. 'UNRIPE'
  bool isEnabled;
  CategorySourceType sourceType;
  String? archetypePresetId;
  bool enableGreenVeto;
  double greenVetoThreshold;
  CategoryKnobs knobs;

  CategoryRule({
    required this.id,
    required this.name,
    this.isEnabled = true,
    this.sourceType = CategorySourceType.archetype,
    this.archetypePresetId = 'prime_ripe',
    this.enableGreenVeto = true,
    this.greenVetoThreshold = 0.35,
    CategoryKnobs? knobs,
  }) : knobs = knobs ?? CategoryKnobs();

  CategoryRule copy() => CategoryRule(
        id: id,
        name: name,
        isEnabled: isEnabled,
        sourceType: sourceType,
        archetypePresetId: archetypePresetId,
        enableGreenVeto: enableGreenVeto,
        greenVetoThreshold: greenVetoThreshold,
        knobs: knobs.copy(),
      );
}

/// The full profile: global balance weights + up-to-5 category slots.
class FruitProfile {
  String name;
  double wOptics;
  double wImage;
  double wMoments;
  List<CategoryRule> categories;

  FruitProfile({
    required this.name,
    this.wOptics = 1.0,
    this.wImage = 1.0,
    this.wMoments = 1.0,
    List<CategoryRule>? categories,
  }) : categories = categories ??
            List.generate(
                RulesModel32D.numClasses,
                (i) => CategoryRule(
                    id: i,
                    name: RulesModel32D.classLabels[i],
                    // UNRIPE (slot 0) is the green-skin class; a green veto
                    // would reject it, so keep it off by default.
                    enableGreenVeto: i != 0));

  int get activeMask {
    var mask = 0;
    for (var i = 0; i < categories.length; i++) {
      if (categories[i].isEnabled) mask |= 1 << i;
    }
    return mask;
  }

  int get activeCount => categories.where((c) => c.isEnabled).length;

  /// Slot labels that are enabled.
  List<String> get activeLabels =>
      categories.where((c) => c.isEnabled).map((c) => c.name).toList();
}

// ── Archetype library ────────────────────────────────────────────────
/// Five empirically-calibrated archetypes with realistic 32-D bases.
/// Each base is a valid non-negative state (block-normalizable) that the
/// compiler block-norms + weights before deploying.
class ArchetypeDef {
  final String id;
  final String displayName;
  final String description;
  final List<double> baseVector;

  /// The intrinsic skin hue (deg) this archetype encodes. Used as the origin
  /// for the hue knob's relative offset when the user synthesises a skin
  /// signature from an archetype source.
  final double naturalHueDeg;

  const ArchetypeDef({
    required this.id,
    required this.displayName,
    required this.description,
    required this.baseVector,
    this.naturalHueDeg = 45.0,
  });
}

class ArchetypeLibrary {
  ArchetypeLibrary._();

  static const List<double> _hardUnripe = [
    0.0000, 0.0010, 0.4990, 0.4990,
    0.0010, 0.0000, 0.0000, 0.0000,
    0.6086, 0.3622, 0.3464, 0.4335,
    0.4225, 0.3386, 0.2424, 0.2238,
    0.2607, 0.2683, 0.1237, 0.0966,
    0.1117, 0.1197, 0.0572, 0.0537,
    0.0825, 0.1022, 0.1905, 0.2985,
    0.7661, 0.6698, 0.3337, 0.0000,
  ];
  static const List<double> _primeRipe = [
    0.7772, 0.2227, 0.0001, 0.0000,
    0.0000, 0.0000, 0.0000, 0.0000,
    0.6903, 0.3551, 0.4156, 0.4698,
    0.4049, 0.3671, 0.2212, 0.1852,
    0.1939, 0.2212, 0.0833, 0.0991,
    0.0973, 0.1266, 0.0811, 0.0769,
    0.0637, 0.1009, 0.0510, 0.0548,
    0.4897, 0.5830, 0.4052, 0.4132,
  ];
  static const List<double> _overripeSoft = [
    0.9859, 0.0141, 0.0000, 0.0000,
    0.0000, 0.0000, 0.0000, 0.0000,
    0.6903, 0.3551, 0.4156, 0.4698,
    0.4049, 0.3671, 0.2212, 0.1852,
    0.1939, 0.2212, 0.0833, 0.0991,
    0.0973, 0.1266, 0.0811, 0.0769,
    0.0637, 0.1009, 0.1510, 0.0000,
    0.4397, 0.1330, 0.4052, 0.6132,
  ];
  static const List<double> _hollowDefect = [
    0.4968, 0.4968, 0.0065, 0.0000,
    0.0000, 0.0000, 0.0000, 0.0000,
    0.6903, 0.3551, 0.5656, 0.4698,
    0.5049, 0.3671, 0.3712, 0.1852,
    0.2939, 0.2212, 0.2333, 0.0991,
    0.1973, 0.1266, 0.2311, 0.0769,
    0.1637, 0.1009, 0.1510, 0.3548,
    0.6397, 0.5830, 0.6552, 0.1632,
  ];
  static const List<double> _inertStandard = [
    0.0028, 0.9775, 0.0197, 0.0000,
    0.0000, 0.0000, 0.0000, 0.0000,
    0.6903, 0.3551, 0.0500, 0.0000,
    0.0000, 1.0000, 0.0500, 0.0000,
    0.0000, 1.0000, 0.0500, 0.0000,
    0.0000, 1.0000, 0.0500, 0.0000,
    0.0000, 1.0000, 0.0500, 0.0200,
    0.6397, 1.0000, 0.1500, 0.0000,
  ];

  static const List<ArchetypeDef> all = [
    ArchetypeDef(
      id: 'hard_unripe',
      displayName: 'Hard Unripe',
      description: 'Stiff flesh, high resonance, hard green skin',
      baseVector: _hardUnripe,
      // Intrinsic hue = weighted mean of dims 0..7 (the base's true skin
      // colour), so the hue-centre label always matches the chart. Drag the
      // knob to 95° to synthesise a deep-green gaussian.
      naturalHueDeg: 57.5,
    ),
    ArchetypeDef(
      id: 'prime_ripe',
      displayName: 'Prime Ripe',
      description: 'Soft pulp, high damping, golden skin',
      baseVector: _primeRipe,
      naturalHueDeg: 29.0,
    ),
    ArchetypeDef(
      id: 'overripe_soft',
      displayName: 'Overripe / Soft',
      description: 'Spongy pulp, extreme energy sink',
      baseVector: _overripeSoft,
      naturalHueDeg: 26.4,
    ),
    ArchetypeDef(
      id: 'hollow_defect',
      displayName: 'Hollow / Internal Void',
      description: 'Air pocket, split resonance, high entropy',
      baseVector: _hollowDefect,
      naturalHueDeg: 32.6,
    ),
    ArchetypeDef(
      id: 'inert_standard',
      displayName: 'Inert / Blank Test',
      description: 'Hard reference block, sharp high-frequency peak',
      baseVector: _inertStandard,
      naturalHueDeg: 39.0,
    ),
  ];

  static ArchetypeDef? byId(String? id) {
    if (id == null) return null;
    for (final a in all) {
      if (a.id == id) return a;
    }
    return null;
  }
}

/// The deterministic 6-step Rule Compiler (spec Section 4) that turns a
/// [FruitProfile] into the 852-byte wire blob.
///
/// v3 compiles to the **Diagonal-Gaussian** parameterization:
///   • `prototypes[c]`  = the raw class MEAN state (block-normalization REMOVED
///     and unit-weighting REMOVED, so acoustic amplitude survives — this is
///     what separates distinct ripe/unripe mangoes).
///   • `feature_weights` = pooled per-feature inverse-variance (precision),
///     estimated from the measured raw states with a small floor and
///     James–Stein shrinkage, then scaled per block by the profile's
///     wOptics/wImage/wMoments (Bayesian precision multipliers).
///
/// [measuredCentroids]: legacy per-label 32-D centroid (used when raw states
/// are unavailable, e.g. for the preview path). [measuredStates]: per-label
/// raw per-tap states used to estimate the pooled variance. Categories without
/// either fall back to their archetype base with a default (unit) precision.
class FruitProfileCompiler {
  FruitProfileCompiler._();

  static const double _varFloor = 1e-4;
  static const double _jsShrink = 0.3;
  static const double _defaultPrecision = 1.0;

  /// Element-wise mean of a list of equal-length state vectors.
  static List<double> _mean(List<List<double>> states) {
    final n = states.length;
    final out = List<double>.filled(RulesModel32D.dims, 0.0);
    if (n == 0) return out;
    for (final s in states) {
      for (var d = 0; d < out.length; d++) {
        out[d] += s[d];
      }
    }
    for (var d = 0; d < out.length; d++) {
      out[d] /= n;
    }
    return out;
  }

  /// Pooled per-feature inverse-variance computed from WITHIN-CLASS variance
  /// (each class's own spread about its own mean), then James–Stein shrunk
  /// toward the median variance (0.3) with a small positive floor. This is the
  /// statistically correct precision for a per-class Diagonal-Gaussian: total
  /// (across-class) variance would inflate the noise on separating features and
  /// crush separation. No measured sample variance -> unit precision (1.0).
  static List<double> _pooledInvVar(
      Map<String, List<List<double>>> statesByClass) {
    final vars = List<double>.filled(RulesModel32D.dims, 0.0);
    var totalDof = 0;

    statesByClass.forEach((name, states) {
      if (states.length < 2) return; // need >= 2 taps for sample variance
      final m = _mean(states);
      for (final s in states) {
        for (var d = 0; d < RulesModel32D.dims; d++) {
          final diff = s[d] - m[d];
          vars[d] += diff * diff;
        }
      }
      totalDof += (states.length - 1);
    });

    // Fallback to unit precision when no sample variance is available (keeps
    // archetype-only model behaviour identical to before — conservative).
    if (totalDof < 1) {
      return List<double>.filled(RulesModel32D.dims, _defaultPrecision);
    }

    final pooled = List<double>.generate(
        RulesModel32D.dims, (d) => vars[d] / totalDof);
    final sortedVars = List<double>.from(pooled)..sort();
    final med = sortedVars[RulesModel32D.dims ~/ 2];

    return List<double>.generate(RulesModel32D.dims, (d) {
      final v = math.max((1.0 - _jsShrink) * pooled[d] + _jsShrink * med,
          _varFloor);
      return 1.0 / v;
    });
  }

  /// Builds each enabled class mean (raw, unnormalized) from measured raw
  /// states, else a measured centroid, else the archetype base, then applies
  /// flesh-dynamic / hue-synthesis deltas.
  static List<List<double>> _classMeans(
      FruitProfile profile,
      Map<String, List<double>> measuredCentroids,
      Map<String, List<List<double>>> measuredStates) {
    final means = List.generate(
        RulesModel32D.numClasses,
        (_) => List<double>.filled(RulesModel32D.dims, 0.0));

    for (final cat in profile.categories) {
      if (!cat.isEnabled) continue;
      final c = cat.id;

      final states = measuredStates[cat.name];
      List<double> base;
      if (states != null && states.isNotEmpty) {
        // Auto-promote: real taps always win over the archetype base, so recorded
        // data is never silently ignored even if the source toggle was left on
        // archetype (the older-architecture UX trap).
        base = _mean(states);
      } else if (cat.sourceType == CategorySourceType.measured &&
          measuredCentroids[cat.name] != null) {
        base = List<double>.from(measuredCentroids[cat.name]!);
      } else {
        base = List<double>.from(
            ArchetypeLibrary.byId(cat.archetypePresetId)?.baseVector ??
                ArchetypeLibrary.all.first.baseVector);
      }

      // Flesh dynamics + optional skin-optics synthesis (unchanged semantics).
      final arch =
          cat.sourceType == CategorySourceType.measured
              ? null
              : ArchetypeLibrary.byId(cat.archetypePresetId) ??
                  ArchetypeLibrary.all.first;
      final syncHue = (cat.knobs.hueDeltaDeg != 0.0);
      final List<double> z;
      if (!syncHue) {
        z = RulesModel32D.applyFleshDeltas(base, cat.knobs);
      } else {
        final natural = arch?.naturalHueDeg ?? 45.0;
        final target =
            RulesModel32D._clamp(natural + cat.knobs.hueDeltaDeg, 20.0, 120.0);
        z = RulesModel32D.applyDeltas(
            base,
            cat.knobs,
            RulesModel32D.hueGaussian(target, _spreadDeg(cat)));
      }

      means[c] = z; // raw: NO block-L2, NO unit normalisation — amplitude kept.
    }

    return means;
  }

  static double _spreadDeg(CategoryRule cat) =>
      1.0 + RulesModel32D._clamp(cat.knobs.hueSpread, 0.0, 1.0) * 59.0;

  static List<double> _biases(FruitProfile profile) {
    return List<double>.generate(
        RulesModel32D.numClasses,
        (c) => (c < profile.categories.length && profile.categories[c].isEnabled)
            ? 0.0
            : -100.0);
  }

  /// Pooled inverse-variance for the enabled classes, scaled per block by the
  /// profile's slider weights (Bayesian precision). wOptics=0 zeroes the
  /// optics block precision (vision ignored), matching the "Skin Optics off".
  static List<double> _precisionWeights(
      FruitProfile profile,
      Map<String, List<double>> measuredCentroids,
      Map<String, List<List<double>>> measuredStates) {
    final statesByClass = <String, List<List<double>>>{};
    for (final cat in profile.categories) {
      if (!cat.isEnabled) continue;
      final states = measuredStates[cat.name];
      if (states != null && states.isNotEmpty) {
        statesByClass[cat.name] = states;
      }
    }
    final iv = _pooledInvVar(statesByClass);
    for (var d = 0; d < 10; d++) iv[d] *= profile.wOptics;
    for (var d = 10; d < 26; d++) iv[d] *= profile.wImage;
    for (var d = 26; d < 32; d++) iv[d] *= profile.wMoments;
    return iv;
  }

  /// Compiles the profile to a ready-to-upload 852-byte blob (v3 Gaussian).
  static Uint8List compile(FruitProfile profile,
      {Map<String, List<double>> measuredCentroids = const {},
      Map<String, List<List<double>>> measuredStates = const {}}) {
    final means = _classMeans(profile, measuredCentroids, measuredStates);
    final invVar = _precisionWeights(profile, measuredCentroids, measuredStates);
    final biases = _biases(profile);
    final mask = profile.activeMask;
    return RulesModel32D.packBinary(
      name: profile.name,
      protos: means,
      biases: biases,
      mask: mask,
      featureWeights: invVar,
      greenVeto: profile.categories
              .any((c) => c.isEnabled && c.enableGreenVeto)
          ? RulesModel32D.greenVetoDefault
          : 0.0,
    );
  }

  /// Pairwise class-separation matrix in Mahalanobis (sigma) units: the raw
  /// d' between the two class means under the shared pooled inverse-variance,
  /// d'(a,b) = sqrt( sum_d invVar[d] * (mu_a[d] - mu_b[d])^2 ). This is the
  /// exact same Gaussian metric the firmware classifier uses, so the on-screen
  /// health margin (min pairwise d') reflects how the balance weights
  /// (wOptics / wImage / wMoments -> invVar) actually separate classes. A d' of
  /// ~1.0 is one standard deviation of between-class distance; higher is better.
  static List<List<double>> mahalanobisMatrix(
      List<List<double>> protos, List<double> invVar) {
    final n = protos.length;
    final m = List.generate(n, (_) => List<double>.filled(n, 0.0));
    for (var i = 0; i < n; i++) {
      for (var j = i + 1; j < n; j++) {
        var sse = 0.0;
        for (var d = 0; d < RulesModel32D.dims; d++) {
          final diff = protos[i][d] - protos[j][d];
          sse += invVar[d] * diff * diff;
        }
        final dPrime = math.sqrt(math.max(0.0, sse));
        m[i][j] = dPrime;
        m[j][i] = dPrime;
      }
    }
    return m;
  }

  /// Returns the compiled class means (for previews / separation matrix).
  static List<List<double>> compiledPrototypes(FruitProfile profile,
      {Map<String, List<double>> measuredCentroids = const {},
      Map<String, List<List<double>>> measuredStates = const {}}) {
    return _classMeans(profile, measuredCentroids, measuredStates);
  }

  /// Returns the compiled per-feature inverse-variance (for preview/selftest).
  static List<double> compiledInverseVariance(FruitProfile profile,
      {Map<String, List<double>> measuredCentroids = const {},
      Map<String, List<List<double>>> measuredStates = const {}}) {
    return _precisionWeights(profile, measuredCentroids, measuredStates);
  }

  static List<double> compiledBiases(FruitProfile profile) =>
      _biases(profile);
}
