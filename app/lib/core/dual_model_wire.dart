import 'dart:convert';
import 'dart:typed_data';

/// Dual-Latent 852-byte wire helpers (DLAT format, esp/main/sci_32d.h).
/// Mirrors RulesModel32D for the new dual-latent model blob.
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

  static const List<String> modalityLabels = ['ACOUSTIC', 'OPTICS', 'DUAL'];
  static const int modalityAcoustic = 0;
  static const int modalityOptics = 1;
  static const int modalityDual = 2;

  static String binName(Uint8List bin) {
    final end = bin.indexWhere((b) => b == 0, 0);
    return String.fromCharCodes(
        bin.sublist(0, end < 0 ? 31 : end).where((b) => b >= 32 && b <= 126));
  }

  static bool isValid(Uint8List bin) {
    if (bin.length != wireBytes) return false;
    // Magic check for DLAT: bytes 32..35 should be 0x44 0x4C 0x41 0x54
    if (bin[32] != 0x44 || bin[33] != 0x4C || bin[34] != 0x41 || bin[35] != 0x54)
      return false;
    if (bin[offsetVersion] != 1) return false;
    final mask = bin[offsetMask];
    if (mask == 0 || (mask & ~(0x1F)) != 0) return false;
    final mod = bin[offsetModality];
    if (mod > 2) return false;
    return true;
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
    out[32] = 0x44; out[33] = 0x4C; out[34] = 0x41; out[35] = 0x54; // DLAT
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
