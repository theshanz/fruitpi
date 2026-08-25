import 'dart:convert';
import 'dart:typed_data';

/// Wire protocol spoken by esp/main/bt_manager.cpp (NimBLE + ArduinoJson).
class BleProtocol {
  BleProtocol._();

  // ── GATT layout (bt_manager.h) ─────────────────────────────────────
  static const serviceUuid = '4fa10001-2241-4cf5-9988-34824317f012';
  static const charModelTransfer = '4fa10002-2241-4cf5-9988-34824317f012';
  static const charScanConfig = '4fa10003-2241-4cf5-9988-34824317f012';
  static const charScanResults = '4fa10004-2241-4cf5-9988-34824317f012';
  static const charRawStream = '4fa10005-2241-4cf5-9988-34824317f012';

  // ── Packet types (bt_manager.h) ────────────────────────────────────
  static const pktJpeg = 0x01;
  static const pktRawWaveform = 0x02;
  static const pktHeader = 0x03;
  static const pktModel = 0x04;
  static const pktPassDone = 0x05;

  static const chunkSize = 500; // payload bytes per chunk
  static const modelWireBytes = 616;
  static const modelWireBytesLegacy = 612;

  // ── Class labels (sci_28d.cpp order) ───────────────────────────────
  static const classLabels = [
    'UNRIPE',
    'PERFECTLY_RIPE',
    'OVERRIPE',
    'ROTTEN_OR_HOLLOW',
    'ARTIFICIALLY_RIPENED',
  ];

  // ── TX helpers (app -> ESP) ────────────────────────────────────────

  /// 8-byte header that starts an upload on the model-transfer char.
  static Uint8List uploadHeader(int id, int total, int type) {
    final b = Uint8List(8);
    b[0] = pktHeader;
    b[1] = (id >> 8) & 0xFF;
    b[2] = id & 0xFF;
    b[3] = (total >> 24) & 0xFF;
    b[4] = (total >> 16) & 0xFF;
    b[5] = (total >> 8) & 0xFF;
    b[6] = total & 0xFF;
    b[7] = type;
    return b;
  }

  /// [type][id_hi][id_lo][seq_hi][seq_lo][end][payload...]
  static Uint8List uploadChunk(int id, int type, int seq, Uint8List data,
      {bool end = false}) {
    final pkt = Uint8List(6 + data.length);
    pkt[0] = type;
    pkt[1] = (id >> 8) & 0xFF;
    pkt[2] = id & 0xFF;
    pkt[3] = (seq >> 8) & 0xFF;
    pkt[4] = seq & 0xFF;
    pkt[5] = end ? 0x01 : 0x00;
    pkt.setRange(6, pkt.length, data);
    return pkt;
  }

  // ── JSON commands (char 03) ────────────────────────────────────────
  static Map<String, dynamic> cmdListModels() => {'command': 'list_models'};
  static Map<String, dynamic> cmdDeleteModel(String fruit) =>
      {'command': 'delete_model', 'fruit': fruit};
  static Map<String, dynamic> cmdActivateModel(String fruit) =>
      {'fruit': fruit};
  static Map<String, dynamic> cmdInference() => {'command': 'inference_request'};
  static Map<String, dynamic> cmdCancel() => {'command': 'cancel'};
  static Map<String, dynamic> cmdMode(String mode) => {'mode': mode};
  static Map<String, dynamic> cmdVisionConfig(
          {double? valueMin, double? satMin, bool save = false}) =>
      {
        'command': 'vision_config',
        if (valueMin != null) 'value_min': valueMin,
        if (satMin != null) 'sat_min': satMin,
        if (save) 'save': true,
      };
}

/// Inference result notified on char 04 (notify_scan_result).
class ScanResultData {
  final String decision;
  final double ripenessIndex;
  final double confidence;
  final double entropy;
  final bool isAnomaly;
  final Map<String, double> probabilities;

  const ScanResultData({
    required this.decision,
    required this.ripenessIndex,
    required this.confidence,
    required this.entropy,
    required this.isAnomaly,
    required this.probabilities,
  });

  static ScanResultData? tryParse(Map<String, dynamic> j) {
    if (!j.containsKey('decision')) return null;
    final probs = <String, double>{};
    final p = j['probabilities'];
    if (p is Map) {
      p.forEach((k, v) => probs[k.toString()] = _d(v));
    }
    return ScanResultData(
      decision: j['decision'].toString(),
      ripenessIndex: _d(j['ripeness_index']),
      confidence: _d(j['confidence']),
      entropy: _d(j['entropy']),
      isAnomaly: j['is_anomaly'] == true || j['is_anomaly'] == 1,
      probabilities: probs,
    );
  }

  static double _d(dynamic v) =>
      v is num ? v.toDouble() : double.tryParse('$v') ?? 0.0;
}

/// Progress of a model upload acknowledged by the firmware.
class TransferProgress {
  final int id;
  final int received;
  final int total;
  const TransferProgress(this.id, this.received, this.total);
  double get fraction => total > 0 ? (received / total).clamp(0.0, 1.0) : 0;
}

/// A completed download streamed from char 05.
class DownloadedTransfer {
  final int type;
  final Uint8List payload;
  const DownloadedTransfer(this.type, this.payload);

  bool get isJpeg => type == BleProtocol.pktJpeg;
  bool get isWaveform => type == BleProtocol.pktRawWaveform;

  /// 512 uint16 LE samples when type == RAW_WAVEFORM.
  List<double> get waveformSamples {
    final bd = ByteData.sublistView(payload);
    final n = payload.length ~/ 2;
    return List<double>.generate(
        n, (i) => bd.getUint16(i * 2, Endian.little).toDouble());
  }
}

/// Parse one notification from char 04 into typed events.
sealed class ResultsEvent {
  static ResultsEvent parse(Uint8List bytes) {
    try {
      final j = jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
      if (j['status'] is String) {
        final status = j['status'] as String;
        if (status == 'transfer_progress') {
          return TransferProgressEvent(
            TransferProgress((j['id'] as num?)?.toInt() ?? 0,
                (j['received'] as num?)?.toInt() ?? 0,
                (j['total'] as num?)?.toInt() ?? 0),
          );
        }
        return StatusEvent(status);
      }
      final result = ScanResultData.tryParse(j);
      if (result != null) return ResultEvent(result);
      if (j['models'] is List) {
        return ModelsListEvent(
          (j['models'] as List).map((e) => e.toString()).toList(),
          active: j['active']?.toString(),
        );
      }
    } catch (_) {}
    return UnknownEvent();
  }
}

class StatusEvent extends ResultsEvent {
  final String status;
  StatusEvent(this.status);
}

class ResultEvent extends ResultsEvent {
  final ScanResultData result;
  ResultEvent(this.result);
}

class ModelsListEvent extends ResultsEvent {
  final List<String> models;
  final String? active;
  ModelsListEvent(this.models, {this.active});
}

class TransferProgressEvent extends ResultsEvent {
  final TransferProgress progress;
  TransferProgressEvent(this.progress);
}

class UnknownEvent extends ResultsEvent {}
