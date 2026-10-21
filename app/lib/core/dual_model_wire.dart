import 'dart:convert';
import 'dart:typed_data';

import 'protocol.dart';

/// Dual-Latent 852-byte wire helpers (DLAT format, esp/main/sci_32d.h).
/// Mirrors RulesModel32D for the new dual-latent model blob.
///
/// WIRE VERSIONS
///   1 — classic latent: one global ripeness latent for every fruit.
///   2 — fruit rules: the previously reserved 728 bytes at offset 124 hold a
///       per-fruit RULE BLOCK (discriminator bands per class). This is the
///       format the Python trainer (datacollection/fruit_rules.py) emits and
///       the format the firmware's evaluate_fruit_rules() consumes.
class DualModelWire {
  DualModelWire._();

  static const wireBytes = 852;
  static const wireBytesLegacy = 848;

  static const offsetName = 0;
  static const offsetMagic = 32;
  static const offsetVersion = 36;
  static const offsetMask = 37;
  static const offsetModality = 38;
  static const offsetGreenStart = 39;
  static const offsetGreenEnd = 40;
  static const offsetR0Spec = 48;
  static const offsetR0Mom = 52;
  static const offsetAlpha = 56;
  static const offsetBeta = 60;
  static const offsetTau = 64;
  static const offsetWAcoustic = 68;
  static const offsetWOptics = 72;
  static const offsetVolLo = 76;
  static const offsetVolHi = 80;
  static const offsetVol23Lo = 84;
  static const offsetVol23Hi = 88;
  static const offsetAnchors = 92;

  // ── v2 rule block ──────────────────────────────────────────────────
  /// The rule block starts where the legacy blob kept its reserved tail.
  static const int ruleOffset = 124;
  static const int ruleBytes = 728;
  static const int wireVersionClassic = 1;
  static const int wireVersionRules = 2;

  static const List<String> modalityLabels = ['ACOUSTIC', 'OPTICS', 'DUAL'];
  static const int modalityAcoustic = 0;
  static const int modalityOptics = 1;
  static const int modalityDual = 2;

  static String binName(Uint8List bin) {
    final end = bin.indexWhere((b) => b == 0, 0);
    return String.fromCharCodes(
        bin.sublist(0, end < 0 ? 31 : end).where((b) => b >= 32 && b <= 126));
  }

  static int wireVersionOf(Uint8List bin) =>
      bin.length >= wireBytes ? bin[offsetVersion] : 0;

  static bool isRulesWire(Uint8List bin) =>
      wireVersionOf(bin) >= wireVersionRules;

  static bool isValid(Uint8List bin) {
    if (bin.length != wireBytes) return false;
    // Magic: the firmware reads a little-endian uint32 and compares it with
    // DUAL_MODEL_MAGIC (0x444C4154), so the bytes on the wire are
    // 54 41 4C 44 ("TALD") — not the ASCII spelling "DLAT".
    if (bin[32] != 0x54 ||
        bin[33] != 0x41 ||
        bin[34] != 0x4C ||
        bin[35] != 0x44) {
      return false;
    }
    final version = bin[offsetVersion];
    if (version != wireVersionClassic && version != wireVersionRules)
      return false;
    final mask = bin[offsetMask];
    if (mask == 0 || (mask & ~(0x1F)) != 0) return false;
    final mod = bin[offsetModality];
    if (mod > 2) return false;
    // v2 additionally carries a rule block the device validates strictly.
    if (version >= wireVersionRules) {
      final rules = DualFruitRules.parse(bin);
      if (rules == null || !rules.isValid) return false;
    } else {
      // The firmware demands an EMPTY rule block on a classic wire.
      if (!isRuleBlockEmpty(bin)) return false;
    }
    return true;
  }

  /// True when the 728-byte rule block is all zeros (a classic v1 wire).
  static bool isRuleBlockEmpty(Uint8List bin) {
    if (bin.length < ruleOffset + ruleBytes) return true;
    for (var i = ruleOffset; i < ruleOffset + ruleBytes; i++) {
      if (bin[i] != 0) return false;
    }
    return true;
  }

  // ── the per-site anchor table ──────────────────────────────────────
  /// The table lives in the rule block's 156 reserved bytes (the blob's
  /// offset 696): a version, a count, and up to 14 rows of 10 bytes
  /// (site_id, flags, anchor, softness).  An all-zero run means the model
  /// has no table and the device picks the site itself.
  static const int sideTableOffset = ruleOffset + 572;
  static const int sideTableBytes = 156;
  static const int sideTableVersion = 1;
  static const int maxSides = 14;
  static const int sideRowBytes = 10;

  /// The strike sites a model was measured on.
  static List<DualModelSite> siteTable(Uint8List bin) {
    if (bin.length < sideTableOffset + sideTableBytes) return const [];
    var allZero = true;
    for (var i = sideTableOffset; i < sideTableOffset + sideTableBytes; i++) {
      if (bin[i] != 0) {
        allZero = false;
        break;
      }
    }
    if (allZero) return const [];
    final version = ByteData.sublistView(
            bin, sideTableOffset, sideTableOffset + 4)
        .getUint32(0, Endian.little);
    if (version != sideTableVersion) return const [];
    final count = bin[sideTableOffset + 4];
    final view = ByteData.sublistView(bin);
    final sites = <DualModelSite>[];
    for (var i = 0; i < count && i < maxSides; i++) {
      final off = sideTableOffset + 8 + i * sideRowBytes;
      sites.add(DualModelSite(
        id: bin[off],
        anchor: view.getFloat32(off + 2, Endian.little),
        softness: view.getFloat32(off + 6, Endian.little),
      ));
    }
    return sites;
  }

  static int maskOf(Uint8List bin) =>
      bin.length >= wireBytes ? bin[offsetMask] : 0x1F;

  static int modalityOf(Uint8List bin) =>
      bin.length >= wireBytes ? bin[offsetModality] : 0;

  static Uint8List withMask(Uint8List bin, int mask) {
    final out = Uint8List.fromList(bin);
    if (out.length < wireBytes) return out;
    out[offsetMask] = mask & 0xFF;
    return out;
  }

  static Uint8List withModality(Uint8List bin, int mod) {
    final out = Uint8List.fromList(bin);
    if (out.length < wireBytes) return out;
    out[offsetModality] = mod.clamp(0, 2);
    // Fix weights per mode (no sliders — toggle fixes them)
    switch (mod) {
      case modalityAcoustic:
        out.setRange(offsetWAcoustic, offsetWAcoustic + 4,
            floatToLeBytes(1.0));
        out.setRange(offsetWOptics, offsetWOptics + 4,
            floatToLeBytes(0.0));
        break;
      case modalityOptics:
        out.setRange(offsetWAcoustic, offsetWAcoustic + 4,
            floatToLeBytes(0.0));
        out.setRange(offsetWOptics, offsetWOptics + 4,
            floatToLeBytes(1.0));
        break;
      case modalityDual:
      default:
        out.setRange(offsetWAcoustic, offsetWAcoustic + 4,
            floatToLeBytes(0.5));
        out.setRange(offsetWOptics, offsetWOptics + 4,
            floatToLeBytes(0.5));
        break;
    }
    return out;
  }

  static Uint8List withName(Uint8List bin, String name) {
    final out = Uint8List.fromList(bin);
    final raw = name.codeUnits.take(31).toList();
    for (var i = 0; i < 32; i++) {
      out[i] = i < raw.length ? raw[i] : 0;
    }
    return out;
  }

  // ── v2 rule block access ───────────────────────────────────────────

  /// The parsed rule block of a v2 blob (null for a v1 blob).
  static DualFruitRules? rulesOf(Uint8List bin) => DualFruitRules.parse(bin);

  /// Write a rule block into the blob and mark the wire as v2.
  static Uint8List withRules(Uint8List bin, DualFruitRules rules) {
    final out = Uint8List.fromList(bin);
    if (out.length < wireBytes) return out;
    out[offsetVersion] = wireVersionRules;
    out.setRange(ruleOffset, ruleOffset + ruleBytes, rules.pack());
    return out;
  }

  /// Rewrite only the global rule knobs (temperature, margin, default class,
  /// anomaly threshold, ripeness gain). Every band is preserved byte for
  /// byte, so a trained model stays trained.
  static Uint8List withRuleKnobs(
    Uint8List bin, {
    double? temperature,
    double? margin,
    int? defaultClass,
    double? anomalyThreshold,
    double? ripenessGain,
  }) {
    final rules = DualFruitRules.parse(bin);
    if (rules == null) return bin;
    return withRules(
        bin,
        rules.copyWith(
          temperature: temperature,
          margin: margin,
          defaultClass: defaultClass,
          anomalyThreshold: anomalyThreshold,
          ripenessGain: ripenessGain,
        ));
  }

  static Uint8List? tryParseBinText(String text) {
    final cleaned = text.replaceAll(RegExp(r'\s+'), '');
    if (cleaned.isEmpty) return null;
    Uint8List? bytes;
    final isHex = cleaned.length.isEven &&
        cleaned.contains(RegExp(r'^[0-9a-fA-F]+$'));
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
    if (bytes.length != wireBytes && bytes.length != wireBytesLegacy) return null;
    return bytes;
  }

  static List<int> floatToLeBytes(double v) {
    final bd = ByteData(4);
    bd.setFloat32(0, v, Endian.little);
    return [bd.getUint8(0), bd.getUint8(1), bd.getUint8(2), bd.getUint8(3)];
  }

  /// Canonical trained constants (mirrors tests/modelexp/build_dual_model_bin.py
  /// DEFAULTS — the live banana multimodal v5 config).
  static const double canonR0Spec = -1.150;
  static const double canonR0Mom = 100.0;
  static const double canonAlpha = 12.0;
  static const double canonBeta = 0.20;
  static const double canonTau = 0.08;
  static const double canonWAcoustic = 0.5;
  static const double canonWOptics = 0.5;
  static const List<double> canonAnchors = [0.10, 0.85, 1.10, 1.30, 0.10];
  static const int canonMinPeakAdc = 160;
  static const int canonFeatMode = 0;
  static const double canonStftT1Prior = 0.5;

  /// Assemble a full 852-byte DLAT wire from editable parameters (the same
  /// layout the Python builder emits). Non-DUAL modalities override the
  /// weights (ACOUSTIC = 1/0, OPTICS = 0/1); DUAL keeps [wAcoustic]/[wOptics].
  static Uint8List build({
    required String name,
    int activeMask = 0x1F,
    int modality = modalityDual,
    int greenStart = 5,
    int greenEnd = 8,
    double r0Spec = canonR0Spec,
    double r0Mom = canonR0Mom,
    double alpha = canonAlpha,
    double beta = canonBeta,
    double tau = canonTau,
    double wAcoustic = canonWAcoustic,
    double wOptics = canonWOptics,
    double volLo = 10.0,
    double volHi = 600.0,
    double vol23Lo = 4.64,
    double vol23Hi = 71.13,
    List<double> anchors = canonAnchors,
    int minPeakAdc = canonMinPeakAdc,
    int featMode = canonFeatMode,
    double stftT1Prior = canonStftT1Prior,
  }) {
    final out = Uint8List(wireBytes);
    final bd = ByteData.sublistView(out);
    final raw = name.codeUnits.take(31).toList();
    for (var i = 0; i < 32; i++) out[i] = i < raw.length ? raw[i] : 0;
    out[32] = 0x54; out[33] = 0x41; out[34] = 0x4C; out[35] = 0x44; // "DLAT" as LE u32
    out[offsetVersion] = 1;
    out[offsetMask] = activeMask & 0xFF;
    out[offsetModality] = modality.clamp(0, 2);
    out[offsetGreenStart] = greenStart & 0xFF;
    out[offsetGreenEnd] = greenEnd & 0xFF;
    bd.setFloat32(offsetR0Spec, r0Spec, Endian.little);
    bd.setFloat32(offsetR0Mom, r0Mom, Endian.little);
    bd.setFloat32(offsetAlpha, alpha, Endian.little);
    bd.setFloat32(offsetBeta, beta, Endian.little);
    bd.setFloat32(offsetTau, tau, Endian.little);
    final (wa, wo) = switch (modality) {
      modalityAcoustic => (1.0, 0.0),
      modalityOptics => (0.0, 1.0),
      _ => (wAcoustic, wOptics),
    };
    bd.setFloat32(offsetWAcoustic, wa, Endian.little);
    bd.setFloat32(offsetWOptics, wo, Endian.little);
    bd.setFloat32(offsetVolLo, volLo, Endian.little);
    bd.setFloat32(offsetVolHi, volHi, Endian.little);
    bd.setFloat32(offsetVol23Lo, vol23Lo, Endian.little);
    bd.setFloat32(offsetVol23Hi, vol23Hi, Endian.little);
    for (var c = 0; c < 5; c++) {
      final a = c < anchors.length ? anchors[c] : 0.5;
      bd.setFloat32(offsetAnchors + 4 * c, a, Endian.little);
    }
    // 112 reserved2_pad stays 0
    bd.setUint16(113, minPeakAdc, Endian.little);
    // 115 reserved2_tail0 stays 0
    out[116] = featMode & 0xFF;
    bd.setFloat32(120, stftT1Prior, Endian.little);
    return out;
  }
}

/// The 19 discriminator features a v2 rule block can use.
/// Mirrors `DiscFeature` in esp/main/fruit_rule.h.
class DualDiscFeature {
  DualDiscFeature._();

  static const int count = 19;
  static const int shear = 0;
  static const int rSpec = 1;
  static const int rMom = 2;
  static const int entropy = 3;
  static const int centroid = 4;
  static const int tail = 5;
  static const int harmonic = 6;
  static const int abbott = 7;
  static const int bioEntropy = 8;
  static const int damping = 9;
  static const int stftT1 = 10;
  static const int peak = 11;
  static const int green = 12;
  static const int warm = 13;
  static const int hueMean = 14;
  static const int dispersion = 15;
  static const int hueEntropy = 16;
  static const int darkSpot = 17;
  static const int darkMass = 18;

  static const List<String> names = [
    'shear', 'r_spec', 'r_mom', 'entropy', 'centroid', 'tail', 'harmonic',
    'abbott', 'bio_entropy', 'damping', 'stft_t1', 'peak', 'green', 'warm',
    'hue_mean', 'dispersion', 'hue_entropy', 'dark_spot', 'dark_mass',
  ];

  static const List<String> descriptions = [
    'FBM-2D shear ratio (median of N taps)',
    'de-coated shear (median of N taps)',
    'moment contrast (median of N taps)',
    'spectral entropy',
    'temporal centroid',
    'tail energy',
    'harmonic absorption',
    'abbott stiffness',
    'bio spectral entropy',
    'dynamic damping',
    'STFT early-decay T1',
    'tap amplitude (median of N taps)',
    'green mass (optics)',
    'warm mass (optics)',
    'mean hue (deg)',
    'chromatic dispersion',
    'hue-histogram entropy',
    'black/brown SPOT area fraction',
    'total dark area fraction',
  ];

  static String name(int id) =>
      (id >= 0 && id < names.length) ? names[id] : 'feat$id';
}

/// The v2 per-fruit RULE BLOCK (728 bytes at wire offset 124).
///
/// Layout (mirrors `FruitRuleBlock` in esp/main/fruit_rule.h and the packer
/// in datacollection/fruit_rules.py):
///
///   0  block_version u32      28 slot_feature[8] u8     76 class_lo[5][8] f32
///   4  slot_mask u32          36 class_slot_mask[5] u8  236 class_hi[5][8] f32
///   8  temperature f32        41 pad0[3]                396 class_soft[5][8] f32
///  12  margin f32             44 slot_weight[8] f32      556 override_class u32
///  16  default_class u32                                  560 override slots/ops
///  20  anomaly_threshold f32                              564 override values
///  24  ripeness_gain f32                                 572 reserved[156]
///
/// A class score is the weighted MEAN of its band memberships over the slots
/// it uses; a slot whose feature could not be measured contributes 0 but
/// still counts in the denominator, so a class that needs a channel we do not
/// have (e.g. the camera for black spots) cannot win.
class DualFruitRules {
  DualFruitRules({
    this.blockVersion = 1,
    this.slotMask = 0,
    this.temperature = 0.12,
    this.margin = 0.05,
    this.defaultClass = noClass,
    this.anomalyThreshold = 0.5,
    this.ripenessGain = 1.0,
    List<int>? slotFeature,
    List<int>? classSlotMask,
    List<double>? slotWeight,
    List<double>? classLo,
    List<double>? classHi,
    List<double>? classSoft,
    this.overrideClass = noClass,
    this.overrideSlot1 = 0,
    this.overrideOp1 = 0,
    this.overrideSlot2 = 0,
    this.overrideOp2 = 0,
    this.overrideVal1 = 0.0,
    this.overrideVal2 = 0.0,
  })  : slotFeature = slotFeature ?? List<int>.filled(8, 0),
        classSlotMask = classSlotMask ?? List<int>.filled(5, 0),
        slotWeight = slotWeight ?? List<double>.filled(8, 0.0),
        classLo = classLo ?? List<double>.filled(40, 0.0),
        classHi = classHi ?? List<double>.filled(40, 0.0),
        classSoft = classSoft ?? List<double>.filled(40, 0.0);

  /// "no class" marker — matches FRM_NO_CLASS in the firmware.
  static const int noClass = 0xFF;
  static const int maxSlots = 8;
  static const int numClasses = 5;

  static const int opGreater = 0;
  static const int opLess = 1;

  final int blockVersion;
  final int slotMask;
  final double temperature;
  final double margin;
  final int defaultClass;
  final double anomalyThreshold;
  final double ripenessGain;
  final List<int> slotFeature;
  final List<int> classSlotMask;
  final List<double> slotWeight;
  final List<double> classLo;
  final List<double> classHi;
  final List<double> classSoft;
  final int overrideClass;
  final int overrideSlot1;
  final int overrideOp1;
  final int overrideSlot2;
  final int overrideOp2;
  final double overrideVal1;
  final double overrideVal2;

  // ── parsing ────────────────────────────────────────────────────────

  /// Parse the rule block of a blob (null when the blob is not a v2 wire).
  static DualFruitRules? parse(Uint8List bin) {
    if (bin.length < DualModelWire.wireBytes) return null;
    if (bin[DualModelWire.offsetVersion] < DualModelWire.wireVersionRules) {
      return null;
    }
    final b = Uint8List.sublistView(
        bin, DualModelWire.ruleOffset, DualModelWire.ruleOffset + 728);
    final bd = ByteData.sublistView(b);
    final slotFeature = <int>[for (var i = 0; i < 8; i++) b[28 + i]];
    final classSlotMask = <int>[for (var i = 0; i < 5; i++) b[36 + i]];
    final slotWeight = <double>[for (var i = 0; i < 8; i++) bd.getFloat32(44 + 4 * i, Endian.little)];
    final classLo = <double>[for (var i = 0; i < 40; i++) bd.getFloat32(76 + 4 * i, Endian.little)];
    final classHi = <double>[for (var i = 0; i < 40; i++) bd.getFloat32(236 + 4 * i, Endian.little)];
    final classSoft = <double>[for (var i = 0; i < 40; i++) bd.getFloat32(396 + 4 * i, Endian.little)];
    return DualFruitRules(
      blockVersion: bd.getUint32(0, Endian.little),
      slotMask: bd.getUint32(4, Endian.little),
      temperature: bd.getFloat32(8, Endian.little),
      margin: bd.getFloat32(12, Endian.little),
      defaultClass: bd.getUint32(16, Endian.little),
      anomalyThreshold: bd.getFloat32(20, Endian.little),
      ripenessGain: bd.getFloat32(24, Endian.little),
      slotFeature: slotFeature,
      classSlotMask: classSlotMask,
      slotWeight: slotWeight,
      classLo: classLo,
      classHi: classHi,
      classSoft: classSoft,
      overrideClass: bd.getUint32(556, Endian.little),
      overrideSlot1: b[560],
      overrideOp1: b[561],
      overrideSlot2: b[562],
      overrideOp2: b[563],
      overrideVal1: bd.getFloat32(564, Endian.little),
      overrideVal2: bd.getFloat32(568, Endian.little),
    );
  }

  // ── packing ────────────────────────────────────────────────────────

  /// The 728-byte canonical block. Padding and the reserved tail are always
  /// zero — the device rejects a blob whose padding is not canonical.
  Uint8List pack() {
    final b = Uint8List(728);
    final bd = ByteData.sublistView(b);
    bd.setUint32(0, blockVersion, Endian.little);
    bd.setUint32(4, slotMask, Endian.little);
    bd.setFloat32(8, temperature, Endian.little);
    bd.setFloat32(12, margin, Endian.little);
    bd.setUint32(16, defaultClass, Endian.little);
    bd.setFloat32(20, anomalyThreshold, Endian.little);
    bd.setFloat32(24, ripenessGain, Endian.little);
    for (var i = 0; i < 8; i++) {
      b[28 + i] = slotFeature[i] & 0xFF;
      bd.setFloat32(44 + 4 * i, slotWeight[i], Endian.little);
    }
    for (var c = 0; c < 5; c++) {
      b[36 + c] = classSlotMask[c] & 0xFF;
      for (var i = 0; i < 8; i++) {
        final k = c * 8 + i;
        bd.setFloat32(76 + 4 * k, classLo[k], Endian.little);
        bd.setFloat32(236 + 4 * k, classHi[k], Endian.little);
        bd.setFloat32(396 + 4 * k, classSoft[k], Endian.little);
      }
    }
    bd.setUint32(556, overrideClass, Endian.little);
    b[560] = overrideSlot1 & 0xFF;
    b[561] = overrideOp1 & 0xFF;
    b[562] = overrideSlot2 & 0xFF;
    b[563] = overrideOp2 & 0xFF;
    bd.setFloat32(564, overrideVal1, Endian.little);
    bd.setFloat32(568, overrideVal2, Endian.little);
    return b;
  }

  DualFruitRules copyWith({
    double? temperature,
    double? margin,
    int? defaultClass,
    double? anomalyThreshold,
    double? ripenessGain,
  }) =>
      DualFruitRules(
        blockVersion: blockVersion,
        slotMask: slotMask,
        temperature: temperature ?? this.temperature,
        margin: margin ?? this.margin,
        defaultClass: defaultClass ?? this.defaultClass,
        anomalyThreshold: anomalyThreshold ?? this.anomalyThreshold,
        ripenessGain: ripenessGain ?? this.ripenessGain,
        slotFeature: List<int>.from(slotFeature),
        classSlotMask: List<int>.from(classSlotMask),
        slotWeight: List<double>.from(slotWeight),
        classLo: List<double>.from(classLo),
        classHi: List<double>.from(classHi),
        classSoft: List<double>.from(classSoft),
        overrideClass: overrideClass,
        overrideSlot1: overrideSlot1,
        overrideOp1: overrideOp1,
        overrideSlot2: overrideSlot2,
        overrideOp2: overrideOp2,
        overrideVal1: overrideVal1,
        overrideVal2: overrideVal2,
      );

  // ── queries ────────────────────────────────────────────────────────

  bool get isEmpty {
    final b = pack();
    for (final v in b) {
      if (v != 0) return false;
    }
    return true;
  }

  /// Slot indices this class uses.
  List<int> slotsOf(int cls) => <int>[
        for (var i = 0; i < maxSlots; i++)
          if ((classSlotMask[cls] >> i) & 1 == 1) i
      ];

  /// Feature id of a slot.
  int featureOf(int slot) => slotFeature[slot];

  /// The band [lo, hi, softness] of a class/slot pair.
  List<double> bandOf(int cls, int slot) {
    final k = cls * maxSlots + slot;
    return [classLo[k], classHi[k], classSoft[k]];
  }

  /// Does this model use the black-spot discriminator?
  bool get usesDarkSpot =>
      List<int>.generate(maxSlots, (i) => i)
          .any((i) => (slotMask >> i) & 1 == 1 &&
              slotFeature[i] == DualDiscFeature.darkSpot);

  /// Does any class use the black-spot discriminator?
  bool get hasSpotRule {
    for (var c = 0; c < numClasses; c++) {
      for (final s in slotsOf(c)) {
        if (slotFeature[s] == DualDiscFeature.darkSpot) return true;
      }
    }
    return false;
  }

  // ── validation (mirror of fruit_rule_block_valid) ─────────────────

  static bool _finite(double v) => v.isFinite;

  /// The device's own validator, mirrored so the app never pushes a blob
  /// the firmware would refuse.
  bool get isValid {
    if (blockVersion != 1) return false;
    if (slotMask == 0) return false;
    if (slotMask & ~((1 << maxSlots) - 1) != 0) return false;
    if (!_finite(temperature) || temperature <= 0) return false;
    if (!_finite(margin) || margin < 0) return false;
    if (!_finite(anomalyThreshold) ||
        anomalyThreshold < 0 ||
        anomalyThreshold > 1) {
      return false;
    }
    if (!_finite(ripenessGain) || ripenessGain < 0) return false;
    if (defaultClass != noClass && defaultClass >= numClasses) return false;

    for (var i = 0; i < maxSlots; i++) {
      final active = (slotMask >> i) & 1 == 1;
      if (!active) {
        if (slotFeature[i] != 0) return false;
        if (slotWeight[i] != 0.0) return false;
        continue;
      }
      if (slotFeature[i] >= DualDiscFeature.count) return false;
      if (!_finite(slotWeight[i]) || slotWeight[i] < 0) return false;
    }

    for (var c = 0; c < numClasses; c++) {
      if (classSlotMask[c] & ~slotMask != 0) return false;
      for (var i = 0; i < maxSlots; i++) {
        final k = c * maxSlots + i;
        final used = (classSlotMask[c] >> i) & 1 == 1;
        if (!used) {
          if (classLo[k] != 0.0 ||
              classHi[k] != 0.0 ||
              classSoft[k] != 0.0) {
            return false;
          }
          continue;
        }
        if (!_finite(classLo[k]) || !_finite(classHi[k])) return false;
        if (!_finite(classSoft[k]) || classSoft[k] <= 0) return false;
        if (classHi[k] < classLo[k]) return false;
      }
    }

    if (overrideClass != noClass) {
      if (overrideClass >= numClasses) return false;
      if (overrideOp1 > opLess || overrideOp2 > opLess) return false;
      if (overrideSlot1 >= DualDiscFeature.count ||
          overrideSlot2 >= DualDiscFeature.count) {
        return false;
      }
      if ((slotMask >> overrideSlot1) & 1 != 1) return false;
      if ((slotMask >> overrideSlot2) & 1 != 1) return false;
      if (!_finite(overrideVal1) || !_finite(overrideVal2)) return false;
    } else {
      if (overrideSlot1 != 0 ||
          overrideSlot2 != 0 ||
          overrideOp1 != 0 ||
          overrideOp2 != 0 ||
          overrideVal1 != 0.0 ||
          overrideVal2 != 0.0) {
        return false;
      }
    }
    return true;
  }
}

/// One strike site a model was measured on: the model's index for it and
/// the class-band edge that applies there.
class DualModelSite {
  final int id;
  final double anchor;
  final double softness;

  const DualModelSite({
    required this.id,
    required this.anchor,
    required this.softness,
  });

  /// The name shown in the picker.  The site ids are the app's own
  /// convention (the order the corpus was captured in), so the names live
  /// in the app rather than in the blob - the blob's table has no room for
  /// them.
  String get label => kSiteNames[id] ?? 'site $id';
}
