import 'dart:async';
import 'dart:convert' show jsonEncode, utf8;
import 'dart:io' show Platform;
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import '../core/protocol.dart';

/// Talks the real FruitPi firmware protocol over BLE.
///
/// GATT map (esp/main/bt_manager.cpp):
///   4fa10001  service
///   4fa10002  model transfer  (WRITE, WRITE_NR)  <- chunked uploads
///   4fa10003  scan config     (READ, WRITE)      <- JSON commands
///   4fa10004  scan results    (NOTIFY)           <- JSON status/results
///   4fa10005  raw stream      (NOTIFY)           <- chunked downloads
class BleService {
  BluetoothDevice? _device;
  BluetoothCharacteristic? _cfgChar;

  BluetoothCharacteristic? _modelChar;
  int _uploadId = 0;

  // ── UI-facing state ────────────────────────────────────────────────
  final connected = ValueNotifier<bool>(false);
  final models = ValueNotifier<List<String>>([]);
  final activeModel = ValueNotifier<String?>(null);

  final _statuses = StreamController<String>.broadcast();
  Stream<String> get statuses => _statuses.stream;

  final _results = StreamController<ScanResultData>.broadcast();
  Stream<ScanResultData> get results => _results.stream;

  final _msCaptured = StreamController<MsCapturedData>.broadcast();
  Stream<MsCapturedData> get msCaptured => _msCaptured.stream;

  final _progress = StreamController<TransferProgress>.broadcast();
  Stream<TransferProgress> get progress => _progress.stream;

  final _downloads = StreamController<DownloadedTransfer>.broadcast();
  Stream<DownloadedTransfer> get downloads => _downloads.stream;

  StreamSubscription? _resultsSub;
  StreamSubscription? _rawSub;
  StreamSubscription? _connStateSub;
  StreamSubscription<List<ScanResult>>? _scanSub;

  // ── Permissions ────────────────────────────────────────────────────
  Future<bool> requestPermissions() async {
    if (Platform.isLinux) return true;
    final statuses = await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.location,
    ].request();
    return statuses.values.every((s) => s.isGranted);
  }

  // ── Scanning ───────────────────────────────────────────────────────
  void startScan({Duration timeout = const Duration(seconds: 8)}) {
    FlutterBluePlus.startScan(timeout: timeout);
  }

  void stopScan() => FlutterBluePlus.stopScan();

  Stream<List<ScanResult>> get scanResults => FlutterBluePlus.scanResults;

  // ── Connection ─────────────────────────────────────────────────────
  Future<bool> connect(BluetoothDevice device) async {
    try {
      await disconnect();
      this._device = device;
      await device.connect(timeout: const Duration(seconds: 12), autoConnect: false);
      if (!Platform.isLinux) {
        try {
          await device.requestMtu(512);
        } catch (_) {}
      }

      final services = await device.discoverServices();
      BluetoothCharacteristic? cfg, res, mdl;
      for (final s in services) {
        if (s.uuid.str != BleProtocol.serviceUuid) continue;
        for (final c in s.characteristics) {
          final u = c.uuid.str;
          if (u == BleProtocol.charScanConfig) cfg = c;
          if (u == BleProtocol.charScanResults) res = c;
          if (u == BleProtocol.charModelTransfer) mdl = c;
        }
      }
      if (cfg == null || res == null) {
        await disconnect();
        return false;
      }
      _cfgChar = cfg;
      _modelChar = mdl;


      await res.setNotifyValue(true);
      _resultsSub?.cancel();
      _resultsSub = res.lastValueStream
          .listen((bytes) => _onResultsNotify(Uint8List.fromList(bytes)));

      final raw = services
          .expand((s) => s.characteristics)
          .firstWhereOrNull(
              (c) => c.uuid.str == BleProtocol.charRawStream);
      if (raw != null) {
        await raw.setNotifyValue(true);
        _rawSub?.cancel();
        _rawSub = raw.lastValueStream
            .listen((bytes) => _onRawNotify(Uint8List.fromList(bytes)));
      }

      connected.value = true;

      // true link-loss detection (device off / out of range) — the OS
      // emits this; we never had it before, so UI stuck on "Connected"
      _connStateSub?.cancel();
      _connStateSub = device.connectionState.listen((st) {
        if (st == BluetoothConnectionState.disconnected) {
          _onLinkLost();
        } else if (st == BluetoothConnectionState.connected) {
          connected.value = true;
        }
      });

      refreshModels(); // fire-and-forget inventory fetch
      return true;
    } catch (_) {
      connected.value = false;
      _device = null;
      return false;
    }
  }

  /// Unexpected loss (walked away / device powered off).
  void _onLinkLost() {
    if (!connected.value) return;
    connected.value = false;
    _resultsSub?.cancel();
    _rawSub?.cancel();
    _resultsSub = null;
    _rawSub = null;
    _cfgChar = null;
    _modelChar = null;
    _rx.clear();
    _statuses.add('device_lost');
  }

  Future<void> disconnect() async {
    _connStateSub?.cancel();
    _connStateSub = null;
    _resultsSub?.cancel();
    _rawSub?.cancel();
    _resultsSub = null;
    _rawSub = null;
    _cfgChar = null;

    _modelChar = null;
    _rx.clear();
    connected.value = false;
    final d = _device;
    _device = null;
    try {
      await d?.disconnect();
    } catch (_) {}
  }

  bool get isConnected => connected.value;
  BluetoothDevice? get device => _device;

  // ── Commands (JSON -> char 03) ─────────────────────────────────────
  Future<bool> sendCommand(Map<String, dynamic> json) async {
    final c = _cfgChar;
    if (c == null || !connected.value) return false;
    try {
      await c.write(utf8Json(json), withoutResponse: false);
      return true;
    } catch (_) {
      return false;
    }
  }

  Future<void> refreshModels() async {
    await sendCommand(BleProtocol.cmdListModels());
  }

  Future<void> activateModel(String name) async {
    await sendCommand(BleProtocol.cmdActivateModel(name));
  }

  Future<void> deleteModel(String name) async {
    await sendCommand(BleProtocol.cmdDeleteModel(name));
  }

  Future<void> startInference() async =>
      sendCommand(BleProtocol.cmdInference());

  Future<void> cancel() async => sendCommand(BleProtocol.cmdCancel());

  // ── Data-collection commands ───────────────────────────────────────
  Future<bool> setDataCollectionMode() async =>
      sendCommand(BleProtocol.cmdMode('data_collection'));

  Future<bool> setInferenceMode() async =>
      sendCommand(BleProtocol.cmdMode('inference'));

  Future<bool> armAcoustic() async =>
      sendCommand(BleProtocol.cmdArmAcoustic());

  /// Second half of the two-stage ready confirmation: after `arm_acoustic`
  /// told the user to place the fruit, this confirms it sits on the piezo so
  /// the firmware actually arms (mirrors the inference flow). Sends
  /// `arm_ready`.
  Future<bool> armReady() async =>
      sendCommand(BleProtocol.cmdArmReady());

  Future<bool> captureMs() async => sendCommand(BleProtocol.cmdMsCapture());

  /// Subscribes a listener to completed raw waveform transfers
  /// (512 uint16 LE samples) — returns an unsubscribe function.
  StreamSubscription<DownloadedTransfer> onWaveform(
          void Function(List<double> samples) cb) =>
      downloads.listen((d) {
        if (d.isWaveform) cb(d.waveformSamples);
      });

  /// Push new black-spot / glare rejection gates to the device.
  Future<void> sendVisionConfig(
          {double? valueMin, double? satMin, bool save = false}) =>
      sendCommand(BleProtocol.cmdVisionConfig(
          valueMin: valueMin, satMin: satMin, save: save));

  // ── Notifications from char 04 ─────────────────────────────────────
  void _onResultsNotify(Uint8List bytes) {
    switch (ResultsEvent.parse(bytes)) {
      case StatusEvent(:final status):
        _statuses.add(status);
        // Firmware never pushes the inventory itself — re-pull on every
        // model lifecycle event so the UI deck stays in sync.
        if (status == 'model_activated' ||
            status == 'model_saved' ||
            status == 'model_deleted') {
          refreshModels();
        }
      case ModelsListEvent(:final models, :final active):
        this.models.value = models;
        activeModel.value = active;
      case ResultEvent(:final result):
        _results.add(result);
      case MsCapturedEvent(:final data):
        _msCaptured.add(data);
      case TransferProgressEvent(:final progress):
        _progress.add(progress);
      case UnknownEvent():
        break;
    }
  }

  // ── Model upload (char 02) ─────────────────────────────────────────
  Completer<bool>? _uploadDone;
  TransferProgress? _lastProgressForId;

  /// Streams [bin] (a 616-byte Fruit28D blob) into NVS.
  /// Resolves true when firmware reports `model_saved`.
  Future<bool> uploadModelBin(Uint8List bin,
      {void Function(double fraction)? onProgress}) async {
    final mdl = _modelChar;
    if (mdl == null || !connected.value) return false;
    if (bin.length < BleProtocol.modelWireBytesLegacy) return false;

    final id = ++_uploadId & 0xFFFF;
    _lastProgressForId = null;
    _uploadDone = Completer<bool>();
    late StreamSubscription progSub;
    progSub = _progress.stream.listen((p) {
      if (p.id != id) return;
      _lastProgressForId = p;
      onProgress?.call(p.fraction);
      if (!_uploadDone!.isCompleted && p.received >= bin.length) {
        // all bytes in — wait briefly for the save verdict below
      }
    });

    // Watch for the save verdict while streaming.
    late StreamSubscription<String> statSub;
    statSub = statuses.listen((s) {
      if (_uploadDone == null || _uploadDone!.isCompleted) return;
      if (s == 'model_saved') {
        _uploadDone!.complete(true);
      } else if (s == 'model_error') {
        _uploadDone!.complete(false);
      }
    });

    try {
      await mdl.write(
          BleProtocol.uploadHeader(id, bin.length, BleProtocol.pktModel),
          withoutResponse: true);

      final chunks =
          (bin.length + BleProtocol.chunkSize - 1) ~/ BleProtocol.chunkSize;
      for (var seq = 0; seq < chunks; seq++) {
        final off = seq * BleProtocol.chunkSize;
        final end =
            (off + BleProtocol.chunkSize > bin.length) ? bin.length : off + BleProtocol.chunkSize;
        final slice = Uint8List.sublistView(bin, off, end);
        await mdl.write(
            BleProtocol.uploadChunk(id, BleProtocol.pktModel, seq, slice,
                end: seq == chunks - 1),
            withoutResponse: true);
        await Future<void>.delayed(const Duration(milliseconds: 25));
      }

      final ok = await _uploadDone!.future
          .timeout(const Duration(seconds: 20), onTimeout: () {
        final p = _lastProgressForId;
        return p != null && p.received >= bin.length;
      });
      return ok;
    } catch (_) {
      return false;
    } finally {
      await progSub.cancel();
      await statSub.cancel();
      _uploadDone = null;
    }
  }

  // ── Downloads from char 05 (JPEG / waveform) ───────────────────────
  final Map<int, _RxTransfer> _rx = {};

  void _onRawNotify(Uint8List bytes) {
    if (bytes.isEmpty) return;
    if (bytes.length >= 8 && bytes[0] == BleProtocol.pktHeader) {
      final id = (bytes[1] << 8) | bytes[2];
      final total = (bytes[3] << 24) | (bytes[4] << 16) | (bytes[5] << 8) | bytes[6];
      final type = bytes[7];
      _rx[id] = _RxTransfer(type, total);
      return;
    }
    if (bytes.length >= 3 && bytes[0] == BleProtocol.pktPassDone) {
      final id = (bytes[1] << 8) | bytes[2];
      final t = _rx[id];
      if (t == null) return;
      final missing = t.missingSeqs();
      if (missing.isEmpty) {
        _finishRx(id, t);
      } else {
        // request retransmission ranges [[start,end],...]
        final ranges = <List<int>>[];
        var i = 0;
        while (i < missing.length) {
          var j = i;
          while (j + 1 < missing.length && missing[j + 1] == missing[j] + 1) {
            j++;
          }
          ranges.add([missing[i], missing[j]]);
          i = j + 1;
        }
        sendCommand({
          'command': 'resend',
          'id': id,
          'ranges': ranges,
        });
      }
      return;
    }
    if (bytes.length >= 6) {
      final type = bytes[0];
      final id = (bytes[1] << 8) | bytes[2];
      final seq = (bytes[3] << 8) | bytes[4];
      final t = _rx[id];
      if (t == null || type != t.type) return;
      t.put(seq, Uint8List.sublistView(bytes, 6));
    }
  }

  void _finishRx(int id, _RxTransfer t) {
    if (!t.complete) return;
    _downloads.add(DownloadedTransfer(t.type, t.assemble()));
    sendCommand({'command': 'transfer_done', 'id': id});
    _rx.remove(id);
  }

  void dispose() {
    _scanSub?.cancel();
    _resultsSub?.cancel();
    _rawSub?.cancel();
    _statuses.close();
    _results.close();
    _progress.close();
    _downloads.close();
    _msCaptured.close();    connected.dispose();
    models.dispose();
    activeModel.dispose();
  }
}

extension _FirstWhere<T> on Iterable<T> {
  T? firstWhereOrNull(bool Function(T) test) {
    for (final e in this) {
      if (test(e)) return e;
    }
    return null;
  }
}

Uint8List utf8Json(Map<String, dynamic> json) =>
    Uint8List.fromList(utf8.encode(jsonEncode(json)));

class _RxTransfer {
  final int type;
  final int total;
  final Map<int, Uint8List> parts = {};

  _RxTransfer(this.type, this.total);

  int get chunkCount => (total + BleProtocol.chunkSize - 1) ~/ BleProtocol.chunkSize;
  bool get complete =>
      parts.length >= chunkCount && parts.keys.every((s) => s < chunkCount);

  void put(int seq, Uint8List data) {
    if (seq * BleProtocol.chunkSize + data.length > total) return;
    parts[seq] = Uint8List.fromList(data);
  }

  List<int> missingSeqs() =>
      List.generate(chunkCount, (i) => i)..removeWhere(parts.containsKey);

  Uint8List assemble() {
    final out = Uint8List(total);
    var off = 0;
    for (var s = 0; s < chunkCount; s++) {
      final p = parts[s];
      if (p == null) continue;
      out.setRange(off, off + p.length, p);
      off += p.length;
    }
    return out;
  }
}
