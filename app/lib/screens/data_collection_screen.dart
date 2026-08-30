import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/material.dart';

import '../core/cozy_palette.dart';
import '../core/protocol.dart';
import '../core/rules_model.dart';
import '../core/training_extractor.dart';
import '../core/training_extractor32d.dart';
import '../core/training_repository.dart';
import '../services/ble_service.dart';
import '../widgets/feature_views.dart';
import '../widgets/frosted.dart';
import '../widgets/spectra_charts.dart';
import '../widgets/volume_selector.dart';
import '../widgets/waveform_chart.dart';

/// Live measured-data collection for one ripeness category.
///
/// Guides the user through the device's real data-collection flow:
///   1 · capture one or more hue scans (ms_capture) → 8-bar histogram each
///   2 · arm the acoustic sensor and tap (any number of times) → raw 512
///      waveforms + the true per-tap FFT spectrum
///
/// Arming is explicitly per-tap (NO auto-loop): after each success the device
/// disarms, so the user presses ARM again and is told to place the fruit
/// before arming. This mirrors the inference flow's placement guard — the
/// 3s disarmed grace window means placing the fruit won't false-trigger.
///
/// Each tap is reduced to firmware-faithful 28-D features and the finished
/// session is stored in [TrainingRepository] (and persisted to a pickable
/// folder on desktop / app docs on mobile). Lives on top of the Rule Builder.
class DataCollectionScreen extends StatefulWidget {
  final BleService bleService;
  final String category;
  final String fruitName;

  /// When editing an already-recorded session, pass it here to preload its
  /// hues/taps so the user can review, delete, rename and re-save.
  final SampleSession? editSession;

  const DataCollectionScreen({
    super.key,
    required this.bleService,
    required this.category,
    required this.fruitName,
    this.editSession,
  });

  @override
  State<DataCollectionScreen> createState() => _DataCollectionScreenState();
}

/// Arming state machine. idle → after ARM pressed, waiting for the device's
/// placement-grace + settle to finish, then acoustic_armed → TAP NOW. A
/// waveform arriving while tapping transitions back to idle (device disarmed).
enum _ArmState { idle, arming, tapNow }

class _DataCollectionScreenState extends State<DataCollectionScreen> {
  final _repo = TrainingRepository.instance;

  final List<HueRecord> _hues = [];
  final List<TapRecord> _taps = [];
  List<double>? _liveWave;

  late final TextEditingController _nameCtrl;

  // Per-session container volume (cm³): acts as the default/nominal volume for
  // this capture session. Threads into the live 32-D preview and the persisted
  // session when no hue scan carries a measured volume.
  double _sessionVolumeCm3 = 350.0;

  _ArmState _armState = _ArmState.idle;
  int _armSeq = 0; // guards against stale re-arm callbacks
  bool _saving = false;
  String? _destLabel;
  bool _autoReArm = false; // user may opt in to re-arm after each tap
  bool _wide = false; // wide layouts show main charts in place of popups
  int? _selectedTapIndex; // row whose spectrum the main chart shows (wide)
  int? _selectedHueIndex; // row whose hue histogram the main chart shows (wide)

  StreamSubscription<DownloadedTransfer>? _waveSub;
  StreamSubscription<MsCapturedData>? _msSub;
  StreamSubscription<String>? _statusSub;

  bool get _isEditing => widget.editSession != null;

  @override
  void initState() {
    super.initState();
    _nameCtrl = TextEditingController(
        text: _isEditing ? widget.editSession!.fruit : widget.fruitName);
    _destLabel = _pickDestLabel();

    if (_isEditing) {
      final e = widget.editSession!;
      _hues.addAll(e.hues);
      _taps.addAll(e.taps);
      if (_hues.isNotEmpty) _sessionVolumeCm3 = _hues.first.volumeCm3;
      if (_taps.isNotEmpty) {
        _liveWave = _taps.last.waveform;
      }
      _latestTip = 'editing ${e.id} — review, edit, rename, then save.';
    } else {
      _latestTip = 'capture a hue scan, then arm & tap.';
    }

    _waveSub = widget.bleService.downloads.listen((d) {
      if (!_isActive) return;
      if (!d.isWaveform) return;
      final wave = d.waveformSamples;
      final f = TrainingExtractor.extractTap(wave);
      final t = TapRecord(
          rawWave: d.payload,
          fftBins: f.fftBins,
          spectrum: TrainingExtractor.spectrumMagnitude(wave),
          entropy: f.entropy,
          impactAmp: f.impactAmp);
      final seq = _armSeq;
      if (!mounted) return;
      setState(() {
        _liveWave = wave;
        _taps.add(t);
        _armState = _ArmState.idle; // device is disarmed after this tap
        _latestTip = 'tap ${_taps.length} captured ✓';
      });
      if (_autoReArm && mounted && seq == _armSeq) {
        _reArm();
      }
    });

    _msSub = widget.bleService.msCaptured.listen((d) {
      if (!mounted) return;
      setState(() {
        _hues.add(HueRecord(
          hueHistogram: List<double>.from(d.hueHistogram),
          chromaticDispersion: d.chromaticDispersion,
          volumeCm3: d.volumeCm3,
        ));
        _latestTip = 'hue #${_hues.length} captured — arm & tap when ready';
      });
    });

    _statusSub = widget.bleService.statuses.listen((s) {
      if (!mounted || !_isActive) return;
      switch (s) {
        case 'place_on_piezo':
        case 'place_fruit':
          if (_armState == _ArmState.arming) {
            setState(() => _latestTip = 'PLACE THE FRUIT then press READY');
          }
          break;
        case 'acoustic_armed':
          if (_armState == _ArmState.arming) {
            setState(() {
              _armState = _ArmState.tapNow;
              _latestTip = 'TAP NOW (device disarms after this tap)';
            });
          }
          break;
        case 'timeout_disarmed':
          if (_armState != _ArmState.idle) {
            setState(() {
              _armState = _ArmState.idle;
              _latestTip = 'timed out — no tap. press ARM to try again';
            });
          }
          break;
        case 'acoustic_already_armed':
          break;
      }
    });

    // Ensure the device is in data-collection mode on entry.
    widget.bleService.setDataCollectionMode();
  }

  bool get _isActive => !_saving && mounted;

  String _pickDestLabel() {
    if (_repo.pickedBaseDir != null) return 'dataset in ${_repo.pickedBaseDir}';
    if (Platform.isLinux) return 'app documents (pick a folder to override)';
    return 'app documents';
  }

  @override
  void dispose() {
    _armSeq++; // invalidate any pending re-arm
    _waveSub?.cancel();
    _msSub?.cancel();
    _statusSub?.cancel();
    _nameCtrl.dispose();
    super.dispose();
  }

  String _latestTip = 'capture a hue scan, then arm & tap.';

  // ── capture controls ───────────────────────────────────────────────
  Future<void> _captureHue() async {
    await widget.bleService.captureMs();
  }

  /// Stage 1 of the two-stage ready confirmation: send `arm_acoustic` so the
  /// firmware asks the user to PLACE the fruit (piezo stays disarmed — placing
  /// can't false-trigger). We wait in [arming] for the user to press READY,
  /// which sends `arm_ready` (stage 2) that makes the firmware actually arm.
  Future<void> _arm() async {
    if (_armState != _ArmState.idle) return;
    final seq = ++_armSeq;
    setState(() {
      _armState = _ArmState.arming;
      _latestTip = 'PLACE THE FRUIT on the piezo, then press READY';
    });
    await widget.bleService.setDataCollectionMode();
    await widget.bleService.armAcoustic();
    if (!mounted || seq != _armSeq) return;
  }

  /// Stage 2: user confirms the fruit is placed → `arm_ready`. The firmware
  /// then arms and notifies `acoustic_armed`, which flips to [tapNow].
  Future<void> _sendReady() async {
    if (_armState != _ArmState.arming) return;
    final seq = _armSeq;
    setState(() {
      _armState = _ArmState.arming;
      _latestTip = 'ready… arming sensor';
    });
    await widget.bleService.armReady();
    if (!mounted || seq != _armSeq) return;
  }

  Future<void> _reArm() async {
    if (!mounted) return;
    await Future<void>.delayed(const Duration(milliseconds: 300));
    if (!mounted) return;
    setState(() {
      _armState = _ArmState.arming;
      _latestTip = 'PLACE THE FRUIT on the piezo, then press READY';
    });
    await widget.bleService.armAcoustic();
  }

  // ── recording list editing ─────────────────────────────────────────
  void _deleteTap(int i) {
    setState(() {
      _taps.removeAt(i);
      if (_taps.isEmpty) _liveWave = null;
      final sel = _selectedTapIndex;
      if (sel != null && (sel == i || sel >= _taps.length)) {
        _selectedTapIndex = null;
      }
      _latestTip = 'tap removed';
    });
  }

  void _deleteHue(int i) {
    setState(() {
      _hues.removeAt(i);
      final sel = _selectedHueIndex;
      if (sel != null && (sel == i || sel >= _hues.length)) {
        _selectedHueIndex = null;
      }
      _latestTip = 'hue capture removed';
    });
  }

  // ── save ───────────────────────────────────────────────────────────
  /// Allow saving with at least 1 tap OR at least 1 hue scan, so a tap-only
  /// (acoustic-only) session can be saved without first taking a photo.
  bool get _canSave => _taps.isNotEmpty || _hues.isNotEmpty;

  Future<void> _saveSession() async {
    if (!_canSave) return;
    // When there are no hue scans (tap-only session), persist nominal defaults
    // (uniform hue, 350 cm³) so the session still forms a valid 32-D profile.
    final effectiveHues = _hues.isNotEmpty
        ? _hues
        : [
            HueRecord(
              hueHistogram: [0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125],
              chromaticDispersion: 1.0,
              volumeCm3: _sessionVolumeCm3,
            ),
          ];
    final fruit = _nameCtrl.text.trim().isEmpty
        ? widget.fruitName
        : _nameCtrl.text.trim();
    _armSeq++; // stop any pending re-arm before saving
    setState(() => _saving = true);
    final repo = _repo;
    // offer folder pick on desktop if none chosen yet
    if (Platform.isLinux && repo.pickedBaseDir == null) {
      final ok = await repo.pickBaseDir();
      if (!ok) {
        setState(() => _saving = false);
        return; // user cancelled the folder dialog
      }
    }
    if (_isEditing) {
      final e = widget.editSession!;
      await repo.updateSession(
        category: e.category,
        id: e.id,
        hues: List.unmodifiable(effectiveHues),
        taps: List.unmodifiable(_taps),
        newFruit: fruit,
      );
    } else {
      await repo.addSession(
        fruit: fruit,
        category: widget.category,
        hues: List.unmodifiable(effectiveHues),
        taps: List.unmodifiable(_taps),
      );
    }
    setState(() => _saving = false);
    if (!mounted) return;
    Navigator.of(context).pop(true);
  }

  // ── preview ────────────────────────────────────────────────────────
  /// Wide-layout tap selection: swap the main spectrum chart to this tap (no
  /// popup). Narrow layouts keep the bottom-sheet preview instead.
  void _selectTap(int i) {
    setState(() {
      _selectedTapIndex = i;
      _selectedHueIndex = null;
      _latestTip = 'showing tap ${i + 1} on the main spectrum chart';
    });
  }

  /// Wide-layout hue selection: swap the main hue chart to this capture.
  void _selectHue(int i) {
    setState(() {
      _selectedHueIndex = i;
      _selectedTapIndex = null;
      _latestTip = 'showing hue ${i + 1} on the main hue chart';
    });
  }

  /// Clear any wide-layout chart selection back to last-tap / aggregate.
  void _clearSelection() {
    setState(() {
      _selectedTapIndex = null;
      _selectedHueIndex = null;
    });
  }
  /// Aggregate hue for the whole session (average across captures). On wide
  /// layouts a tapped hue row swaps it to that single capture's histogram.
  List<double> _avgHistogram() {
    if (_selectedHueIndex != null && _selectedHueIndex! < _hues.length) {
      return List<double>.from(_hues[_selectedHueIndex!].hueHistogram);
    }
    if (_hues.isEmpty) return List<double>.filled(8, 0.125);
    final hist = List<double>.filled(8, 0);
    for (final h in _hues) {
      for (var i = 0; i < 8; i++) {
        hist[i] += h.hueHistogram[i];
      }
    }
    for (var i = 0; i < 8; i++) {
      hist[i] /= _hues.length;
    }
    return hist;
  }

  Color get _color =>
      Cozy.accents[RulesModel.classLabels.indexOf(widget.category) %
          Cozy.accents.length];

  /// Session-average volume (cm³); falls back to the per-session volume when
  /// no hue captured yet.
  double get _avgVolume {
    if (_hues.isEmpty) return _sessionVolumeCm3;
    var vol = 0.0;
    for (final h in _hues) {
      vol += h.volumeCm3;
    }
    return vol / _hues.length;
  }

  /// Live 32-D acoustic fingerprint of the most recent tap: the 4x4
  /// spectrogram (16D) + 6 bio-moment bars, extracted in real time from the
  /// captured waveform via the same balanced 32-D extractor as the firmware.
  /// Rendered next to the raw waveform so every tap is visibly grounded in the
  /// actual measured physics, not a bare squiggle.
  Widget _buildLive32DCard(TapRecord lastTap) {
    final state32d = TrainingExtractor32D.extractState32dBalanced(
      wave: lastTap.waveform,
      visionHues: _avgHistogram(),
      volumeCm3: _avgVolume,
    );
    final spectrogramValues = state32d.sublist(10, 26);
    final bioMomentValues = state32d.sublist(26, 32);

    return Container(
      margin: const EdgeInsets.symmetric(vertical: 10),
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: const Color(0xFF14161B),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: Colors.white.withValues(alpha: 0.08)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisSize: MainAxisSize.min,
        children: [
          Row(children: [
            const Icon(Icons.fingerprint_rounded,
                size: 14, color: Cozy.matcha),
            const SizedBox(width: 6),
            const Text(
              'EXTRACTED 32D SIGNATURE (THIS TAP)',
              style: TextStyle(
                fontSize: 12,
                fontWeight: FontWeight.bold,
                letterSpacing: 0.8,
                color: Cozy.matcha,
              ),
            ),
          ]),
          const SizedBox(height: 8),
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              // Left: 4x4 Spectrogram Heatmap
              Expanded(
                flex: 5,
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    const Text('4x4 SPECTROGRAM (16D)',
                        style: TextStyle(fontSize: 12, color: Cozy.dimGray)),
                    const SizedBox(height: 4),
                    SpectrogramGrid(
                      values: spectrogramValues,
                      color: Cozy.matcha,
                    ),
                  ],
                ),
              ),
              const SizedBox(width: 14),
              // Right: 6 Bio-Moment Bars
              Expanded(
                flex: 6,
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    const Text('FLESH DYNAMICS (6D)',
                        style: TextStyle(fontSize: 12, color: Cozy.dimGray)),
                    const SizedBox(height: 4),
                    FleshDynamicsBars(
                      values: bioMomentValues,
                      color: Cozy.chamomile,
                      labels: const [
                        'cen', 'tail', 'harm', 'stiff', 'entr', 'damp'],
                    ),
                  ],
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }

  // ── UI ─────────────────────────────────────────────────────────────
  @override
  Widget build(BuildContext context) {
    _wide = MediaQuery.sizeOf(context).width > 920;
    final hueChartWidget = HueHistogramChart(
        series: PlotSeries(widget.category, _color, _avgHistogram()),
        height: 120);

    return Scaffold(
      appBar: AppBar(
        title: Text('DATA · ${widget.category.replaceAll('_', ' ')}'),
        actions: [
          IconButton(
              tooltip: 'Close',
              icon: const Icon(Icons.close_rounded),
              onPressed: () {
                _armSeq++;
                Navigator.of(context).pop();
              }),
        ],
      ),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
          children: [
            // destination
            FrostedBox(
              padding: const EdgeInsets.all(14),
              borderRadius: BorderRadius.circular(18),
              child: Row(children: [
                const Icon(Icons.folder_rounded, size: 18, color: Cozy.duskBlue),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(_isEditing ? 'EDITING SESSION' : 'SESSION DESTINATION',
                          style: TextStyle(
                              fontSize: 13, letterSpacing: 1.2, color: Cozy.dimGray)),
                      const SizedBox(height: 3),
                      Text('${_nameCtrl.text} · ${widget.category}',
                          style: const TextStyle(
                              fontSize: 16,
                              fontWeight: FontWeight.bold,
                              color: Cozy.oatmeal)),
                      const SizedBox(height: 2),
                      Text(_destLabel ?? '',
                          style: const TextStyle(fontSize: 13.5, color: Cozy.warmGray)),
                    ],
                  ),
                ),
                if (Platform.isLinux)
                  TextButton.icon(
                    onPressed: () async {
                      final ok = await _repo.pickBaseDir();
                      if (ok) setState(() => _destLabel = _pickDestLabel());
                    },
                    icon: const Icon(Icons.create_new_folder_rounded, size: 15),
                    label: const Text('PICK FOLDER'),
                  ),
              ]),
            ),
            const SizedBox(height: 14),

            // fruit name (renamed on save)
            TextField(
              controller: _nameCtrl,
              maxLength: 31,
              style: const TextStyle(fontFamily: Cozy.monoFamily, fontSize: 13),
              decoration: InputDecoration(
                labelText: _isEditing ? 'FRUIT NAME (RENAME)' : 'FRUIT NAME',
                labelStyle: TextStyle(fontSize: 15, color: Cozy.dimGray),
                prefixIcon:
                    const Icon(Icons.local_florist_rounded, color: Cozy.matcha),
                filled: true,
                fillColor: Colors.white.withValues(alpha: 0.03),
                border: OutlineInputBorder(
                    borderRadius: BorderRadius.circular(16),
                    borderSide: BorderSide(
                        color: Colors.white.withValues(alpha: 0.08))),
                focusedBorder: OutlineInputBorder(
                    borderRadius: BorderRadius.circular(16),
                    borderSide: const BorderSide(color: Cozy.matcha)),
              ),
              onChanged: (_) => setState(() {}),
            ),
            const SizedBox(height: 14),

            // per-session container volume
            FrostedBox(
              padding: const EdgeInsets.all(14),
              borderRadius: BorderRadius.circular(18),
              child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const SectionLabel(title: '// SESSION VOLUME'),
                    const SizedBox(height: 4),
                    const Text(
                        'used for this session’s live 32-D preview and any '
                        'tap-only save',
                        style: TextStyle(
                            fontSize: 13.5, color: Cozy.dimGray)),
                    const SizedBox(height: 12),
                    VolumeSelector(
                      volumeCm3: _sessionVolumeCm3,
                      onChanged: (v) =>
                          setState(() => _sessionVolumeCm3 = v),
                    ),
                  ]),
            ),
            const SizedBox(height: 14),

            // hue captures (multiple)
            FrostedBox(
              padding: const EdgeInsets.all(14),
              borderRadius: BorderRadius.circular(18),
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Row(children: [
                  const SectionLabel(title: '// 1 · HUE SCANS'),
                  const Spacer(),
                  if (_wide && _selectedHueIndex != null) ...[
                    GestureDetector(
                      onTap: _clearSelection,
                      child: Container(
                        padding: const EdgeInsets.symmetric(
                            horizontal: 8, vertical: 3),
                        decoration: BoxDecoration(
                          color: Cozy.duskBlue.withValues(alpha: 0.14),
                          borderRadius: BorderRadius.circular(8),
                        ),
                        child: const Text('SHOW AVG',
                            style: TextStyle(
                                fontSize: 13, color: Cozy.duskBlue)),
                      ),
                    ),
                    const SizedBox(width: 8),
                  ],
                  Text('${_hues.length} captured',
                      style: const TextStyle(fontSize: 13.5, color: Cozy.dimGray)),
                ]),
                const SizedBox(height: 10),
                hueChartWidget,
                const SizedBox(height: 10),
                FilledButton.icon(
                  style: FilledButton.styleFrom(
                      backgroundColor: Cozy.duskBlue,
                      foregroundColor: Cozy.deepBg),
                  onPressed: _captureHue,
                  icon: const Icon(Icons.hdr_weak_rounded, size: 18),
                  label: Text(_hues.isEmpty ? 'CAPTURE HUE' : 'ADD HUE SCAN',
                      style: const TextStyle(fontWeight: FontWeight.bold)),
                ),
                if (_hues.isNotEmpty) ...[
                  const SizedBox(height: 10),
                  _hueList(),
                ],
              ]),
            ),
            const SizedBox(height: 14),

            // acoustic taps (unlimited, one explicit arm per tap)
            FrostedBox(
              padding: const EdgeInsets.all(14),
              borderRadius: BorderRadius.circular(18),
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Row(children: [
                  const SectionLabel(title: '// 2 · ACOUSTIC TAPS'),
                  const Spacer(),
                  Text('${_taps.length} taps',
                      style: TextStyle(
                          fontFamily: Cozy.monoFamily,
                          fontSize: 16,
                          fontWeight: FontWeight.bold,
                          color: Cozy.oatmeal)),
                ]),
                const SizedBox(height: 6),
                AnimatedSwitcher(
                  duration: const Duration(milliseconds: 250),
                  child: Text(_latestTip,
                      key: ValueKey(_latestTip),
                      style: TextStyle(
                          fontSize: 14.5,
                          height: 1.4,
                          color: _armState == _ArmState.tapNow
                              ? Cozy.chamomile
                              : Cozy.warmGray)),
                ),
                const SizedBox(height: 4),
                Row(children: [
                  Text('AUTO RE-ARM',
                      style: TextStyle(fontSize: 13.5, color: Cozy.dimGray)),
                  const Spacer(),
                  Switch(
                    value: _autoReArm,
                    activeColor: Cozy.chamomile,
                    onChanged: _armState == _ArmState.idle
                        ? (v) => setState(() => _autoReArm = v)
                        : null,
                  ),
                ]),
                const SizedBox(height: 12),
                SizedBox(
                    height: 140,
                    child: WaveformChart(samples: _liveWave, height: 140)),
                if (_taps.isNotEmpty) _buildLive32DCard(_taps.last),
                const SizedBox(height: 12),
                Row(children: [
                  Expanded(
                    child: OutlinedButton.icon(
                      style: OutlinedButton.styleFrom(
                        side: BorderSide(
                            color: _armState == _ArmState.tapNow
                                ? Cozy.roseError
                                : Cozy.warmGray),
                        foregroundColor:
                            _armState == _ArmState.tapNow
                                ? Cozy.roseError
                                : Cozy.warmGray,
                      ),
                      onPressed: _armState == _ArmState.idle
                          ? _arm
                          : _armState == _ArmState.arming
                              ? _sendReady
                              : null,
                      icon: Icon(_armState == _ArmState.arming
                          ? Icons.task_alt_rounded
                          : Icons.radar_rounded),
                      label: Text(
                          _armState == _ArmState.idle
                              ? _taps.isEmpty
                                  ? 'ARM & TAP'
                                  : 'TAP AGAIN'
                              : _armState == _ArmState.arming
                                  ? 'I’M READY'
                                  : 'TAP NOW',
                          style: const TextStyle(fontWeight: FontWeight.bold)),
                    ),
                  ),
                  const SizedBox(width: 10),
                  Expanded(
                    child: FilledButton.icon(
                      style: FilledButton.styleFrom(
                        backgroundColor: Cozy.matcha,
                        foregroundColor: Cozy.deepBg,
                      ),
                      onPressed: _saving || !_canSave ? null : _saveSession,
                      icon: _saving
                          ? const SizedBox(
                              width: 16,
                              height: 16,
                              child: CircularProgressIndicator(
                                  strokeWidth: 2, color: Cozy.deepBg))
                          : const Icon(Icons.save_rounded, size: 18),
                      label: const Text('SAVE SESSION',
                          style: TextStyle(fontWeight: FontWeight.bold)),
                    ),
                  ),
                ]),
              ]),
            ),
            const SizedBox(height: 14),

            // scrollable, deleteable recording list
            if (_taps.isNotEmpty || _hues.isNotEmpty)
              FrostedBox(
                padding: const EdgeInsets.all(14),
                borderRadius: BorderRadius.circular(18),
                child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const SectionLabel(title: '// RECORDINGS'),
                      const SizedBox(height: 8),
                      _recordingsList(),
                    ]),
              ),
          ],
        ),
      ),
    );
  }

  Widget _hueList() {
    return Column(children: [
      for (var i = 0; i < _hues.length; i++)
        Padding(
          padding: const EdgeInsets.only(bottom: 4),
          child: Row(children: [
            Icon(Icons.colorize_rounded,
                size: 14, color: Cozy.accents[i % Cozy.accents.length]),
            const SizedBox(width: 8),
            Expanded(
                child: Text(
                    'hue ${i + 1} · '
                    '${_hues[i].chromaticDispersion.toStringAsFixed(2)} · '
                    '${_hues[i].volumeCm3.toStringAsFixed(0)} cm³',
                    style: const TextStyle(
                        fontSize: 14.5, color: Cozy.oatmeal))),
            // aggregate shown as average (per-capture detail is in each file)
            IconButton(
              visualDensity: VisualDensity.compact,
              icon: const Icon(Icons.delete_outline_rounded,
                  size: 16, color: Cozy.roseError),
              onPressed: () => _deleteHue(i),
            ),
          ]),
        ),
    ]);
  }

  /// Scrollable list of taps (and hues) so the user can review and delete
  /// any recording before saving. On wide layouts tapping a row swaps the
  /// main chart in place; on narrow layouts it opens a bottom-sheet preview.
  Widget _recordingsList() {
    final rows = <Widget>[];
    for (var i = 0; i < _hues.length; i++) {
      rows.add(_recRow(
          key: 'h${i + 1}',
          icon: Icons.colorize_rounded,
          color: Cozy.accents[i % Cozy.accents.length],
          text: 'hue ${i + 1} · '
              '${_hues[i].chromaticDispersion.toStringAsFixed(2)} · '
              '${_hues[i].volumeCm3.toStringAsFixed(0)} cm³',
          selected: _wide && _selectedHueIndex == i,
          onDelete: _hues.length == 1 && _taps.isEmpty ? null : () => _deleteHue(i),
          onTap: _wide ? () => _selectHue(i) : () => _previewHue(i)));
    }
    for (var i = 0; i < _taps.length; i++) {
      final f = _taps[i];
      rows.add(_recRow(
          key: 't${i + 1}',
          icon: Icons.graphic_eq_rounded,
          color: Cozy.matcha,
          text: 'tap ${i + 1} · '
              'peak ${f.impactAmp.toStringAsFixed(4)} · '
              'entropy ${f.entropy.toStringAsFixed(3)}',
          selected: _wide && _selectedTapIndex == i,
          onDelete: _taps.length == 1 && _hues.isEmpty
              ? null
              : () => _deleteTap(i),
          onTap: _wide ? () => _selectTap(i) : () => _previewTap(i)));
    }
    if (rows.isEmpty) return const SizedBox.shrink();
    return ConstrainedBox(
      constraints: const BoxConstraints(maxHeight: 220),
      child: ListView.separated(
        shrinkWrap: true,
        itemCount: rows.length,
        separatorBuilder: (_, __) => const Divider(
            height: 1, color: Colors.white12),
        itemBuilder: (_, i) => rows[i],
      ),
    );
  }

  /// Preview a recorded tap: its raw waveform + raw FFT spectrum.
  void _previewTap(int i) {
    final t = _taps[i];
    showModalBottomSheet<void>(
      context: context,
      backgroundColor: Cozy.surfaceCard,
      isScrollControlled: true,
      shape: const RoundedRectangleBorder(
          borderRadius: BorderRadius.vertical(top: Radius.circular(22))),
      builder: (sheet) {
        final spec = PlotSeries(
            widget.category, Cozy.matcha, List<double>.from(t.spectrum));
        return SafeArea(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(18, 16, 18, 24),
            child: Column(mainAxisSize: MainAxisSize.min, children: [
              Row(children: [
                const Icon(Icons.graphic_eq_rounded, size: 18, color: Cozy.matcha),
                const SizedBox(width: 8),
                Text('TAP ${i + 1} PREVIEW',
                    style: const TextStyle(
                        fontFamily: Cozy.monoFamily,
                        fontSize: 13,
                        fontWeight: FontWeight.bold,
                        color: Cozy.oatmeal)),
                const Spacer(),
                IconButton(
                  icon: const Icon(Icons.close_rounded, color: Cozy.dimGray),
                  onPressed: () => Navigator.of(sheet).pop(),
                ),
              ]),
              const SizedBox(height: 10),
              const SectionLabel(title: '// RAW WAVEFORM (ADC)'),
              const SizedBox(height: 6),
              SizedBox(
                  height: 120, child: WaveformChart(samples: t.waveform)),
              const SizedBox(height: 14),
              const SectionLabel(title: '// TAP SPECTRUM (FFT magnitude)'),
              const SizedBox(height: 6),
              SizedBox(height: 130, child: SpectrumChart(series: spec)),
            ]),
          ),
        );
      },
    );
  }

  /// Preview a recorded hue capture: its 8-bin histogram + dispersion/volume.
  void _previewHue(int i) {
    final h = _hues[i];
    showModalBottomSheet<void>(
      context: context,
      backgroundColor: Cozy.surfaceCard,
      isScrollControlled: true,
      shape: const RoundedRectangleBorder(
          borderRadius: BorderRadius.vertical(top: Radius.circular(22))),
      builder: (sheet) {
        final series = PlotSeries(
            widget.category, Cozy.accents[i % Cozy.accents.length],
            List<double>.from(h.hueHistogram));
        return SafeArea(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(18, 16, 18, 24),
            child: Column(mainAxisSize: MainAxisSize.min, children: [
              Row(children: [
                Icon(Icons.colorize_rounded,
                    size: 18,
                    color: Cozy.accents[i % Cozy.accents.length]),
                const SizedBox(width: 8),
                Text('HUE ${i + 1} PREVIEW',
                    style: const TextStyle(
                        fontFamily: Cozy.monoFamily,
                        fontSize: 13,
                        fontWeight: FontWeight.bold,
                        color: Cozy.oatmeal)),
                const Spacer(),
                IconButton(
                  icon: const Icon(Icons.close_rounded, color: Cozy.dimGray),
                  onPressed: () => Navigator.of(sheet).pop(),
                ),
              ]),
              const SizedBox(height: 10),
              SizedBox(height: 150, child: HueHistogramChart(series: series)),
              const SizedBox(height: 12),
              Row(children: [
                _previewStat('DISPERSION',
                    h.chromaticDispersion.toStringAsFixed(3), Cozy.duskBlue),
                const SizedBox(width: 10),
                _previewStat('VOLUME', '${h.volumeCm3.toStringAsFixed(0)} cm³',
                    Cozy.chamomile),
              ]),
            ]),
          ),
        );
      },
    );
  }

  Widget _previewStat(String label, String value, Color color) {
    return Expanded(
      child: Container(
        padding: const EdgeInsets.symmetric(vertical: 10, horizontal: 12),
        decoration: BoxDecoration(
          color: Colors.white.withValues(alpha: 0.04),
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: color.withValues(alpha: 0.35)),
        ),
        child: Column(children: [
          Text(label,
              style: const TextStyle(fontSize: 12.5, letterSpacing: 1.2, color: Cozy.dimGray)),
          const SizedBox(height: 4),
          Text(value,
              style: TextStyle(
                  fontFamily: Cozy.monoFamily,
                  fontSize: 14,
                  fontWeight: FontWeight.bold,
                  color: color)),
        ]),
      ),
    );
  }

  Widget _recRow({
    required String key,
    required IconData icon,
    required Color color,
    required String text,
    bool selected = false,
    VoidCallback? onDelete,
    VoidCallback? onTap,
  }) {
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(8),
      child: Container(
        decoration: selected
            ? BoxDecoration(
                color: color.withValues(alpha: 0.12),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: color.withValues(alpha: 0.55)),
              )
            : null,
        padding: const EdgeInsets.symmetric(vertical: 4, horizontal: 6),
        child: Row(children: [
          Icon(icon, size: 14, color: color),
          const SizedBox(width: 8),
          Expanded(
              child: Text(text,
                  style: TextStyle(
                      fontSize: 14.5,
                      color: Cozy.oatmeal,
                      fontWeight: selected ? FontWeight.bold : FontWeight.normal))),
          if (onTap != null && !_wide)
            const Icon(Icons.chevron_right_rounded, size: 15, color: Cozy.dimGray),
          IconButton(
            visualDensity: VisualDensity.compact,
            icon: const Icon(Icons.delete_outline_rounded,
                size: 16, color: Cozy.roseError),
            onPressed: onDelete,
          ),
        ]),
      ),
    );
  }
}
