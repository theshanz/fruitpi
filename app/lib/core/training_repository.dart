import 'dart:async';
import 'dart:convert' show jsonEncode, jsonDecode;
import 'dart:io';
import 'dart:typed_data' show Uint8List, ByteData, Endian;

import 'package:file_picker/file_picker.dart';
import 'package:flutter/foundation.dart';
import 'package:path_provider/path_provider.dart';

import 'rules_model.dart';
import 'training_extractor.dart';

/// One multispectral hue capture (ms_captured): histogram + dispersion + volume.
@immutable
class HueRecord {
  final List<double> hueHistogram; // 8
  final double chromaticDispersion;
  final double volumeCm3;

  const HueRecord({
    required this.hueHistogram,
    required this.chromaticDispersion,
    required this.volumeCm3,
  });
}

/// One acoustic tap captured during a measured session. Stores the raw
/// firmware-faithful FFT features so the aggregated prototype (which applies
/// the force-invariant correction once, to the averaged bins) matches
/// extract_28d.py exactly, plus the dense log-magnitude spectrum used for
/// live/visual spectrum plotting.
@immutable
class TapRecord {
  final Uint8List rawWave; // 512 uint16 LE ADC counts
  final List<double> fftBins; // 15 (force-invariant-scaled, model features)
  final List<double> spectrum; // dense log-magnitude FFT curve (150–2100 Hz)
  final double entropy;
  final double impactAmp;

  const TapRecord({
    required this.rawWave,
    required this.fftBins,
    required this.spectrum,
    required this.entropy,
    required this.impactAmp,
  });

  /// 512 ADC counts as doubles (uint16 LE decode of [rawWave]).
  List<double> get waveform {
    final bd = ByteData.sublistView(rawWave);
    return List<double>.generate(
        rawWave.length ~/ 2, (i) => bd.getUint16(i * 2, Endian.little).toDouble());
  }
}

/// One measured session for a category: any number of hue captures + an
/// unlimited number of taps. Persisted to `dataset/<fruit>/<sample_id>/`
/// mirroring collectorrrr.py (one hue_0N.json per hue, one waveform_NN.csv
/// per tap).
@immutable
class SampleSession {
  final String id; // "<fruit>_<unixTs>"
  final String category;
  final List<HueRecord> hues; // >= 0 (multiple allowed)
  final List<TapRecord> taps; // >= 0 (unlimited, cannot auto-loop)
  final DateTime timestamp;

  const SampleSession({
    required this.id,
    required this.category,
    required this.hues,
    required this.taps,
    required this.timestamp,
  });

  String get fruit => id.contains('_') ? id.split('_').first : id;
  int get tapCount => taps.length;
  int get hueCount => hues.length;

  /// Aggregate hue (average of every hue histogram / dispersion / volume) —
  /// mirrors extract_28d's averaging across hue_*.json files.
  HueRecord? get aggregateHue {
    if (hues.isEmpty) return null;
    var hist = List<double>.filled(8, 0);
    var disp = 0.0, vol = 0.0;
    for (final h in hues) {
      for (var i = 0; i < 8; i++) {
        hist[i] += h.hueHistogram[i];
      }
      disp += h.chromaticDispersion;
      vol += h.volumeCm3;
    }
    final n = hues.length.toDouble();
    for (var i = 0; i < 8; i++) {
      hist[i] /= n;
    }
    return HueRecord(
      hueHistogram: hist,
      chromaticDispersion: disp / n,
      volumeCm3: vol / n,
    );
  }
}

/// In-memory per-category measured training data + persistence.
///
/// Sessions live in-app for the current run (like ModelVault); each is also
/// written to disk — on desktop to a folder the user picks via file_picker
/// (`dataset/<fruit>/<sample_id>/...`), on mobile to the app documents dir.
class TrainingRepository extends ChangeNotifier {
  TrainingRepository._();
  static final TrainingRepository instance = TrainingRepository._();

  final Map<String, List<SampleSession>> _sessions = {};
  String? _pickedBaseDir;

  Map<String, List<SampleSession>> get sessions => _sessions;

  /// Categories that have at least one connected session.
  Set<String> get connectedCategories => _sessions.entries
      .where((e) => e.value.isNotEmpty)
      .map((e) => e.key)
      .toSet();

  List<SampleSession> sessionsFor(String category) =>
      List.unmodifiable(_sessions[category] ?? const []);

  bool hasData(String category) => (_sessions[category]?.isNotEmpty) ?? false;

  void setPickedBaseDir(String dir) {
    _pickedBaseDir = dir;
    notifyListeners();
  }

  String? get pickedBaseDir => _pickedBaseDir;

  /// Let the user choose a desktop folder for dataset persistence.
  /// Returns false if cancelled; true (with [pickedBaseDir] set) on success.
  Future<bool> pickBaseDir() async {
    try {
      final path = await FilePicker.getDirectoryPath(
          dialogTitle: 'Choose a dataset folder (writes dataset/<fruit>/...)');
      if (path == null || path.isEmpty) return false;
      _pickedBaseDir = path;
      notifyListeners();
      return true;
    } catch (_) {
      return false;
    }
  }

  /// Default mobile location: <app-docs>/fruitpi_dataset.
  Future<Directory> _defaultDocsDir() async {
    final base = await getApplicationDocumentsDirectory();
    final dir = Directory('${base.path}/fruitpi_dataset');
    if (!dir.existsSync()) dir.createSync(recursive: true);
    return dir;
  }

  // ── Session lifecycle ──────────────────────────────────────────────

  /// Register a completed capture as a session for [category], persist it,
  /// and return the session (null if there is neither a hue nor a tap).
  Future<SampleSession?> addSession({
    required String fruit,
    required String category,
    required List<HueRecord> hues,
    required List<TapRecord> taps,
    bool persist = true,
  }) async {
    if (hues.isEmpty && taps.isEmpty) return null;
    final id = '${fruit.trim()}_${DateTime.now().millisecondsSinceEpoch ~/ 1000}';
    final s = SampleSession(
      id: id,
      category: category,
      hues: List.unmodifiable(hues),
      taps: List.unmodifiable(taps),
      timestamp: DateTime.now(),
    );
    (_sessions[category] ??= []).add(s);
    notifyListeners();

    if (persist) {
      try {
        await _persist(s, fruit.trim());
      } catch (_) {}
    }
    return s;
  }

  void removeSession(String category, String id) {
    final list = _sessions[category];
    if (list == null) return;
    list.removeWhere((s) => s.id == id);
    if (list.isEmpty) _sessions.remove(category);
    notifyListeners();
  }

  /// Replace an existing session in memory + on disk with new hues/taps, and
  /// optionally rename its fruit/category. The old on-disk folder is removed
  /// and a fresh one is written (so edits = one updated session, not a dup).
  Future<SampleSession?> updateSession({
    required String category,
    required String id,
    required List<HueRecord> hues,
    required List<TapRecord> taps,
    String? newFruit,
    String? newCategory,
  }) async {
    SampleSession? old;
    final list = _sessions[category];
    if (list != null) {
      for (final e in list) {
        if (e.id == id) {
          old = e;
          break;
        }
      }
    }
    if (old == null || list == null) return null;

    list.remove(old);
    if (list.isEmpty) _sessions.remove(category);
    try {
      final dir = await _sessionDir(old.fruit, old.id);
      if (dir.existsSync()) dir.deleteSync(recursive: true);
    } catch (_) {}

    final cat = newCategory?.trim().isNotEmpty == true
        ? newCategory!.trim()
        : category;
    final fruit = newFruit?.trim().isNotEmpty == true ? newFruit!.trim() : old.fruit;
    final ns = SampleSession(
      id: '${fruit}_${DateTime.now().millisecondsSinceEpoch ~/ 1000}',
      category: cat,
      hues: List.unmodifiable(hues),
      taps: List.unmodifiable(taps),
      timestamp: DateTime.now(),
    );
    (_sessions[cat] ??= []).add(ns);
    try {
      await _persist(ns, fruit);
    } catch (_) {}
    notifyListeners();
    return ns;
  }

  /// Scan the persisted dataset folder (picked dir on desktop, app docs on
  /// mobile) and pull any `hue_0N.json` + `waveform_NN.csv` + metadata back
  /// into memory as sessions, idempotent by session id. Returns the number
  /// of sessions newly loaded.
  Future<int> loadFromDisk() async {
    final base = _pickedBaseDir != null
        ? Directory(_pickedBaseDir!)
        : await _defaultDocsDir();
    final dataset = Directory('${base.path}/dataset');
    if (!dataset.existsSync()) return 0;

    final knownIds = {
      for (final list in _sessions.values)
        for (final s in list) s.id,
    };
    final loaded = <SampleSession>[];
    for (final fruitDir in dataset.listSync().whereType<Directory>()) {
      final fruit = fruitDir.path
          .split(Platform.pathSeparator)
          .last;
      for (final sDir in fruitDir.listSync().whereType<Directory>()) {
        final meta = File('${sDir.path}/metadata.json');
        if (!meta.existsSync()) continue;
        final s = _readSessionDir(sDir, fruit);
        if (s == null || knownIds.contains(s.id)) continue;
        knownIds.add(s.id);
        loaded.add(s);
      }
    }
    for (final s in loaded) {
      (_sessions[s.category] ??= []).add(s);
    }
    if (loaded.isNotEmpty) notifyListeners();
    return loaded.length;
  }

  /// Parse one persisted session folder back into a [SampleSession].
  SampleSession? _readSessionDir(Directory sDir, String fruit) {
    try {
      final meta =
          jsonDecode(File('${sDir.path}/metadata.json').readAsStringSync())
              as Map<String, dynamic>;
      final category = meta['category'] as String? ?? '';
      if (category.isEmpty) return null;
      final id = meta['sample_id'] as String? ??
          sDir.path.split(Platform.pathSeparator).last;

      final hues = <HueRecord>[];
      var hueIdx = 1;
      while (true) {
        final f = File(
            '${sDir.path}/hue_${hueIdx.toString().padLeft(2, '0')}.json');
        if (!f.existsSync()) break;
        final m = jsonDecode(f.readAsStringSync()) as Map<String, dynamic>;
        hues.add(HueRecord(
          hueHistogram: List<double>.from(
              ((m['hue_histogram'] as List?) ?? const [])
                  .map((x) => (x as num).toDouble())),
          chromaticDispersion:
              (m['chromatic_dispersion'] as num?)?.toDouble() ?? 1.0,
          volumeCm3: (m['volume_cm3'] as num?)?.toDouble() ?? 150.0,
        ));
        hueIdx++;
      }

      final taps = <TapRecord>[];
      var tapIdx = 1;
      while (true) {
        final f = File(
            '${sDir.path}/waveform_${tapIdx.toString().padLeft(2, '0')}.csv');
        if (!f.existsSync()) break;
        final wave = f
            .readAsStringSync()
            .trim()
            .split(',')
            .where((x) => x.isNotEmpty)
            .map((x) => double.tryParse(x) ?? 0)
            .toList();
        if (wave.isNotEmpty) taps.add(_tapFromWave(wave));
        tapIdx++;
      }

      return SampleSession(
        id: id,
        category: category,
        hues: List.unmodifiable(hues),
        taps: List.unmodifiable(taps),
        timestamp: DateTime.now(),
      );
    } catch (_) {
      return null;
    }
  }

  /// Rebuild a [TapRecord] from a persisted waveform (ADC counts). The FFT
  /// features are recomputed firmware-faithfully, matching what the live tap
  /// would have produced.
  TapRecord _tapFromWave(List<double> wave) {
    final f = TrainingExtractor.extractTap(wave);
    final raw = Uint8List(wave.length * 2);
    final bd = ByteData.sublistView(raw);
    for (var i = 0; i < wave.length; i++) {
      bd.setUint16(
          i * 2, wave[i].round().clamp(0, 65535), Endian.little);
    }
    return TapRecord(
        rawWave: raw,
        fftBins: f.fftBins,
        spectrum: TrainingExtractor.spectrumMagnitude(wave),
        entropy: f.entropy,
        impactAmp: f.impactAmp);
  }

  void clearAll() {
    _sessions.clear();
    notifyListeners();
  }

  // ── Prototype computation ──────────────────────────────────────────

  /// Robust mean 28-D prototype across every tap + every hue capture of
  /// every session of [category]. Aggregates raw FFT bins + hue exactly like
  /// extract_28d (averages all hue_*.json and all waveform_*.csv).
  List<double>? prototypeOf(String category) {
    final list = _sessions[category];
    if (list == null || list.isEmpty) return null;

    final taps = <({List<double> fftBins, double entropy, double impactAmp})>[];
    final hueRecs = <HueRecord>[];
    for (final s in list) {
      for (final t in s.taps) {
        taps.add((fftBins: t.fftBins, entropy: t.entropy, impactAmp: t.impactAmp));
      }
      hueRecs.addAll(s.hues);
    }
    if (taps.isEmpty || hueRecs.isEmpty) return null; // need both spectra + hue

    // average hue across every hue capture (histogram + scalars)
    var hist = List<double>.filled(8, 0);
    var disp = 0.0, vol = 0.0;
    for (final h in hueRecs) {
      for (var i = 0; i < 8; i++) {
        hist[i] += h.hueHistogram[i];
      }
      disp += h.chromaticDispersion;
      vol += h.volumeCm3;
    }
    final n = hueRecs.length.toDouble();
    for (var i = 0; i < 8; i++) {
      hist[i] /= n;
    }
    disp /= n;
    vol /= n;

    return TrainingExtractor.buildState(
      hueHistogram: hist,
      chromaticDispersion: disp,
      volumeCm3: vol,
      taps: taps,
    );
  }

  /// prototypes for the enabled categories that have connected data.
  Map<String, List<double>> prototypesFor(Iterable<String> enabledLabels) {
    final out = <String, List<double>>{};
    for (final l in enabledLabels) {
      if (!hasData(l)) continue;
      final p = prototypeOf(l);
      if (p != null) out[l] = p;
    }
    return out;
  }

  /// Train ALL connected categories into one 616-byte model via the same
  /// Dart path the Rule Builder uses (range floor + auto-temp baked in).
  Uint8List? trainAll({
    required String fruitName,
    required Iterable<String> enabledLabels,
    double tempFactor = 1.0,
  }) {
    final protos = prototypesFor(enabledLabels);
    if (protos.length < 2) return null; // need >= 2 classes to separate
    final built = RulesModel.buildFromPrototypes(
        protos, enabledLabels.toList(),
        tempFactor: tempFactor);
    return RulesModel.packBinary(fruitName.trim(), built.w, built.b, built.mask);
  }

  // ── Persistence ────────────────────────────────────────────────────

  /// Target directory for a session's files: picked base (desktop) else
  /// mobile docs, then `dataset/<fruit>/<id>/`.
  Future<Directory> _sessionDir(String fruit, String id) async {
    final base = _pickedBaseDir != null
        ? Directory(_pickedBaseDir!)
        : await _defaultDocsDir();
    final dir = Directory('${base.path}/dataset/$fruit/$id');
    if (!dir.existsSync()) dir.createSync(recursive: true);
    return dir;
  }

  Future<void> _persist(SampleSession s, String fruit) async {
    final dir = await _sessionDir(fruit, s.id);

    // waveform_NN.csv — raw ADC counts, one tap per file (unlimited taps)
    for (var i = 0; i < s.taps.length; i++) {
      final wave = s.taps[i].waveform.map((v) => v.round().toString()).join(',');
      File('${dir.path}/waveform_${(i + 1).toString().padLeft(2, '0')}.csv')
          .writeAsStringSync(wave);
    }

    // hue_01..N.json — one per hue capture, schema extract_28d.py reads back
    for (var i = 0; i < s.hues.length; i++) {
      final h = s.hues[i];
      File('${dir.path}/hue_${(i + 1).toString().padLeft(2, '0')}.json')
          .writeAsStringSync(jsonEncode({
        'hue_histogram': h.hueHistogram,
        'chromatic_dispersion': h.chromaticDispersion,
        'volume_cm3': h.volumeCm3,
      }));
    }

    // metadata.json
    File('${dir.path}/metadata.json').writeAsStringSync(jsonEncode({
      'fruit_type': fruit,
      'category': s.category,
      'sample_id': s.id,
      'volume_cm3': s.aggregateHue?.volumeCm3,
      'num_taps': s.taps.length,
      'num_hues': s.hues.length,
      'timestamp':
          s.timestamp.toIso8601String().replaceAll('T', ' ').split('.').first,
    }));
  }

  /// Absolute path of the persisted folder for a session (for UI display).
  Future<String?> persistedPath(String category, String id) async {
    final s = (_sessions[category] ?? const []).where((x) => x.id == id).firstOrNull;
    if (s == null) return null;
    try {
      final dir = await _sessionDir(s.fruit, s.id);
      return dir.existsSync() ? dir.path : null;
    } catch (_) {
      return null;
    }
  }
}

extension _FirstOrNull<T> on Iterable<T> {
  T? get firstOrNull {
    for (final e in this) {
      return e;
    }
    return null;
  }
}
