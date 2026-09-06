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
}
