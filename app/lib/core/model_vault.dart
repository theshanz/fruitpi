import 'dart:convert' show utf8;
import 'dart:typed_data';

/// Session-scoped vault: remembers bins the app itself produced, so models
/// can be re-edited without firmware read-back.
class ModelVault {
  ModelVault._();

  static final _bins = <String, Uint8List>{};

  static void put(Uint8List bin) {
    final name = keyOf(bin);
    if (name.isNotEmpty) _bins[name] = Uint8List.fromList(bin);
  }

  static Uint8List? get(String name) => _bins[name.toLowerCase().trim()];

  static String keyOf(Uint8List bin) {
    final end = bin.indexWhere((b) => b == 0, 0);
    final raw =
        utf8.decode(bin.sublist(0, end < 0 ? 31 : end), allowMalformed: true);
    return raw.toLowerCase().trim();
  }
}
