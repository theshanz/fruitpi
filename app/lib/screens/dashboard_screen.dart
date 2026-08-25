import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/cozy_palette.dart';
import '../core/model_vault.dart';
import '../core/protocol.dart';
import '../services/ble_service.dart';
import '../widgets/frosted.dart';
import 'add_model_flow.dart';
import 'devices_screen.dart';
import 'vision_config_sheet.dart';
import 'model_editor_screen.dart';

class CozySpectraDashboard extends StatefulWidget {
  final BleService bleService;
  const CozySpectraDashboard({super.key, required this.bleService});

  @override
  State<CozySpectraDashboard> createState() => _CozySpectraDashboardState();
}

class _CozySpectraDashboardState extends State<CozySpectraDashboard>
    with SingleTickerProviderStateMixin {
  late AnimationController _cursorController;
  int _selected = 0;

  ScanResultData? _result;
  String _phase = '';
  bool _running = false;
  StreamSubscription? _statusSub;
  StreamSubscription? _resultSub;

  /// firmware status -> terminal copy
  static const _phaseCopy = {
    'place_fruit': 'HOLD FRUIT TO CAMERA…',
    'place_on_piezo': 'PLACE ON PIEZO · CONFIRM WITH SCAN',
    'acoustic_armed': 'TAP THE FRUIT NOW',
    'acoustic_captured': 'TAP CAPTURED — ANALYZING',
    'place_on_piezo_timeout': 'PLACEMENT WINDOW TIMED OUT',
    'timeout_disarmed': 'SESSION TIMED OUT',
    'disarmed': 'CANCELLED',
    'no_model': 'NO ACTIVE MODEL — ENROLL ONE FIRST',
    'camera_error': 'CAMERA FAULT ON DEVICE',
    'device_lost': 'LINK LOST — DEVICE DISCONNECTED',
  };

  Color get _accent =>
      Cozy.accents[_selected % Cozy.accents.length];

  @override
  void initState() {
    super.initState();
    _cursorController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 650),
    )..repeat(reverse: true);

    _statusSub = widget.bleService.statuses.listen((s) {
      if (!mounted) return;
      setState(() {
        _phase = s;
        // terminal statuses end the run — otherwise the banner sticks
        // and the scan button stays dead
        const terminal = {
          'acoustic_captured', 'disarmed', 'no_model', 'camera_error',
          'model_saved', 'model_deleted', 'model_activated', 'model_cleared',
          'model_error',
        };
        if (terminal.contains(s) || s.startsWith('timeout')) _running = false;
        if (_phaseCopy[s] == null) _phase = '';
      });
    });
    _resultSub = widget.bleService.results.listen((r) {
      if (!mounted) return;
      setState(() {
        _result = r;
        _running = false;
        _phase = '';
      });
    });
  }

  @override
  void dispose() {
    _cursorController.dispose();
    _statusSub?.cancel();
    _resultSub?.cancel();
    super.dispose();
  }

  // ── Actions ────────────────────────────────────────────────────────
  void _openDevices() async {
    widget.bleService.stopScan();
    await Navigator.push(context,
        MaterialPageRoute(builder: (_) => DevicesScreen(bleService: widget.bleService)));
  }

  void _startScan() {
    if (!widget.bleService.isConnected || _running) return;
    setState(() => _running = true);
    widget.bleService.startInference();
  }

  void _cancelScan() {
    widget.bleService.cancel();
    setState(() => _running = false);
  }

  Future<void> _activateModel(String name) async {
    HapticFeedback.selectionClick();
    await widget.bleService.activateModel(name);
  }

  Future<void> _deleteModel(String name) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: Cozy.surfaceCard,
        title: Text('delete "$name"?',
            style: const TextStyle(fontFamily: Cozy.monoFamily, fontSize: 15)),
        content: Text('erased from ESP32 flash permanently.',
            style: TextStyle(fontSize: 12, color: Cozy.warmGray)),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(ctx, false),
              child: const Text('KEEP')),
          FilledButton(
            style: FilledButton.styleFrom(backgroundColor: Cozy.roseError),
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('DELETE'),
          ),
        ],
      ),
    );
    if (ok == true) await widget.bleService.deleteModel(name);
  }

  Future<void> _modelActions(String name) async {
    final action = await showModalBottomSheet<String>(
      context: context,
      showDragHandle: true,
      backgroundColor: Cozy.surfaceCard,
      builder: (ctx) => SafeArea(
        child: Padding(
          padding: const EdgeInsets.fromLTRB(16, 0, 16, 20),
          child: Column(mainAxisSize: MainAxisSize.min, children: [
            Align(
              alignment: Alignment.centerLeft,
              child: SectionLabel(
                  title: '// ${name.toUpperCase()} — ACTIONS'),
            ),
            const SizedBox(height: 10),
            ListTile(
              leading: const Icon(Icons.play_circle_outline_rounded,
                  color: Cozy.matcha),
              title: const Text('ACTIVATE',
                  style:
                      TextStyle(fontSize: 12, fontWeight: FontWeight.bold)),
              onTap: () {
                Navigator.pop(ctx);
                _activateModel(name);
              },
            ),
            ListTile(
              leading: const Icon(Icons.edit_rounded, color: Cozy.chamomile),
              title: const Text('EDIT (rename / classes / sharpness)',
                  style:
                      TextStyle(fontSize: 12, fontWeight: FontWeight.bold)),
              onTap: () => Navigator.pop(ctx, 'edit'),
            ),
            ListTile(
              leading:
                  const Icon(Icons.delete_outline_rounded, color: Cozy.roseError),
              title: const Text('DELETE',
                  style:
                      TextStyle(fontSize: 12, fontWeight: FontWeight.bold)),
              onTap: () {
                Navigator.pop(ctx);
                _deleteModel(name);
              },
            ),
          ]),
        ),
      ),
    );

    if (action != 'edit' || !mounted) return;

    var bin = ModelVault.get(name);
    if (bin == null && mounted) {
      // no local copy — ask user to paste the original .bin
      bin = await showPasteBinDialog(context);
    }
    if (bin == null || !mounted) return;
    final edited = await Navigator.push<Uint8List>(
      context,
      MaterialPageRoute(
          builder: (_) =>
              ModelEditorScreen(bleService: widget.bleService, originalBin: bin!)),
    );
    if (edited != null && mounted) {
      await uploadBinWithProgress(context, widget.bleService, edited);
    }
  }

  // ── Build ──────────────────────────────────────────────────────────
  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Stack(children: [
        Positioned(top: -50, left: -40, child: AmbientGlow(color: Cozy.matcha.withValues(alpha: 0.12), size: 360)),
        Positioned(top: 240, right: -60, child: AmbientGlow(color: Cozy.chamomile.withValues(alpha: 0.09), size: 380)),
        Positioned(bottom: 40, left: 20, child: AmbientGlow(color: Cozy.heatherPink.withValues(alpha: 0.08), size: 340)),
        SafeArea(
          child: SingleChildScrollView(
            physics: const BouncingScrollPhysics(),
            padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 16),
            child: Center(
              child: ConstrainedBox(
                constraints: const BoxConstraints(maxWidth: 1050),
                child: LayoutBuilder(builder: (context, cons) {
                  final wide = cons.maxWidth > 850;
                  final phaseText = _phaseCopy[_phase];

                  Widget phaseBanner() => phaseText == null
                      ? const SizedBox.shrink()
                      : Padding(
                          padding: const EdgeInsets.only(bottom: 16),
                          child: FrostedBox(
                            padding: const EdgeInsets.all(14),
                            borderColor: _accent.withValues(alpha: 0.4),
                            backgroundColor:
                                _accent.withValues(alpha: 0.07),
                            child: Row(children: [
                              SizedBox(
                                  width: 20,
                                  height: 20,
                                  child: CircularProgressIndicator(
                                      strokeWidth: 2.5, color: _accent)),
                              const SizedBox(width: 12),
                              Expanded(
                                child: Text(phaseText.toUpperCase(),
                                    style: TextStyle(
                                        fontSize: 11,
                                        letterSpacing: 1.2,
                                        fontWeight: FontWeight.bold,
                                        color: _accent)),
                              ),
                              GestureDetector(
                                onTap: _cancelScan,
                                child: const Icon(Icons.close_rounded,
                                    size: 18, color: Cozy.dimGray),
                              ),
                            ]),
                          ),
                        );

                  final deckColumn = Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
                        const SectionLabel(title: '// SPECIMEN DECK · MODELS'),
                        Row(children: [
                          ValueListenableBuilder<List<String>>(
                            valueListenable: widget.bleService.models,
                            builder: (_, models, __) => Text(
                                models.isEmpty
                                    ? '0/0'
                                    : '${_selected + 1}/${models.length}',
                                style: TextStyle(
                                    color: _accent,
                                    fontSize: 12,
                                    fontWeight: FontWeight.bold)),
                          ),
                          const SizedBox(width: 12),
                          GestureDetector(
                            onTap: () =>
                                showAddModelFlow(context, widget.bleService),
                            child: Container(
                              padding: const EdgeInsets.symmetric(
                                  horizontal: 10, vertical: 5),
                              decoration: BoxDecoration(
                                color:
                                    Colors.white.withValues(alpha: 0.05),
                                borderRadius: BorderRadius.circular(8),
                                border: Border.all(
                                    color: Colors.white
                                        .withValues(alpha: 0.08)),
                              ),
                              child: Row(children: const [
                                Icon(Icons.add,
                                    color: Cozy.matcha, size: 14),
                                SizedBox(width: 4),
                                Text('ENROLL',
                                    style: TextStyle(
                                        color: Cozy.matcha,
                                        fontSize: 10,
                                        fontWeight: FontWeight.bold)),
                              ]),
                            ),
                          ),
                        ]),
                      ]),
                      const SizedBox(height: 12),
                      _modelDeck(),
                    ],
                  );

                  final readoutColumn = Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      phaseBanner(),
                      if (_result != null)
                        _realResultCard(_result!)
                      else if (phaseText == null)
                        FrostedBox(
                          child: SizedBox(
                            width: double.infinity,
                            child: Padding(
                              padding:
                                  const EdgeInsets.symmetric(vertical: 26),
                              child: Column(children: [
                                Icon(Icons.sensors_rounded,
                                    size: 38, color: Cozy.dimGray),
                                const SizedBox(height: 10),
                                Text('SPECTROMETER READY.',
                                    textAlign: TextAlign.center,
                                    style: TextStyle(
                                        fontSize: 11, color: Cozy.dimGray)),
                              ]),
                            ),
                          ),
                        ),
                      const SizedBox(height: 16),
                      _scanButton(),
                    ],
                  );

                  return Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      _headerBar(),
                      const SizedBox(height: 24),
                      if (wide) ...[
                        Row(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Expanded(flex: 6, child: deckColumn),
                            const SizedBox(width: 20),
                            Expanded(flex: 5, child: readoutColumn),
                          ],
                        ),
                      ] else ...[
                        deckColumn,
                        const SizedBox(height: 20),
                        readoutColumn,
                      ],
                      const SizedBox(height: 24),
                    ],
                  );
                }),
              ),
            ),
          ),
        ),
      ]),
    );
  }

  Widget _headerBar() {
    return FrostedBox(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
      borderRadius: BorderRadius.circular(20),
      child: Wrap(
        spacing: 12,
        runSpacing: 10,
        alignment: WrapAlignment.spaceBetween,
        crossAxisAlignment: WrapCrossAlignment.center,
        children: [
          Row(mainAxisSize: MainAxisSize.min, children: [
            const Text('fruitipi@harvest:~#',
                style: TextStyle(
                    color: Cozy.matcha, fontWeight: FontWeight.bold, fontSize: 13)),
            FadeTransition(
              opacity: _cursorController,
              child: Container(
                width: 7,
                height: 14,
                margin: const EdgeInsets.only(left: 4),
                decoration: BoxDecoration(
                    color: Cozy.matcha, borderRadius: BorderRadius.circular(2)),
              ),
            ),
          ]),
          GestureDetector(
            onTap: () => showVisionConfigSheet(context, widget.bleService),
            child: Container(
              padding: const EdgeInsets.all(6),
              decoration: BoxDecoration(
                color: Colors.white.withValues(alpha: 0.05),
                borderRadius: BorderRadius.circular(10),
                border: Border.all(color: Colors.white.withValues(alpha: 0.08)),
              ),
              child: const Icon(Icons.tune_rounded, size: 16, color: Cozy.chamomile),
            ),
          ),
          const SizedBox(width: 8),
          GestureDetector(
            onTap: () => widget.bleService.refreshModels(),
            child: Container(
              padding: const EdgeInsets.all(6),
              decoration: BoxDecoration(
                color: Colors.white.withValues(alpha: 0.05),
                borderRadius: BorderRadius.circular(10),
                border: Border.all(color: Colors.white.withValues(alpha: 0.08)),
              ),
              child: const Icon(Icons.refresh_rounded, size: 16, color: Cozy.matcha),
            ),
          ),
          const SizedBox(width: 10),
          ValueListenableBuilder<bool>(
            valueListenable: widget.bleService.connected,
            builder: (_, connected, __) => Row(mainAxisSize: MainAxisSize.min, children: [
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                decoration: BoxDecoration(
                  color: connected
                      ? Cozy.matcha.withValues(alpha: 0.12)
                      : Colors.white.withValues(alpha: 0.05),
                  borderRadius: BorderRadius.circular(12),
                ),
                child: Row(mainAxisSize: MainAxisSize.min, children: [
                  Icon(Icons.circle, size: 8,
                      color: connected ? Cozy.matcha : Cozy.roseError),
                  const SizedBox(width: 6),
                  Text(connected ? 'FRUITPI_BLE [READY]' : 'UNLINKED',
                      style: TextStyle(
                          fontSize: 10,
                          fontWeight: FontWeight.bold,
                          color: connected ? Cozy.matcha : Cozy.warmGray)),
                ]),
              ),
              const SizedBox(width: 10),
              GestureDetector(
                onTap: _openDevices,
                child: Container(
                  padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                  decoration: BoxDecoration(
                    color: connected
                        ? Colors.white.withValues(alpha: 0.08)
                        : Cozy.matcha,
                    borderRadius: BorderRadius.circular(10),
                  ),
                  child: Text(connected ? 'DEVICES' : 'CONNECT',
                      style: TextStyle(
                          fontSize: 10,
                          fontWeight: FontWeight.bold,
                          color: connected ? Cozy.oatmeal : Cozy.deepBg)),
                ),
              ),
            ]),
          ),
        ],
      ),
    );
  }

  Widget _modelDeck() {
    return ValueListenableBuilder<List<String>>(
      valueListenable: widget.bleService.models,
      builder: (context, models, _) {
        if (models.isEmpty) {
          return FrostedBox(
            borderRadius: BorderRadius.circular(26),
            child: SizedBox(
              width: double.infinity,
              child: Column(children: [
                Icon(Icons.inventory_2_outlined, size: 40, color: Cozy.dimGray),
                const SizedBox(height: 10),
                Text('NO MODELS ON DEVICE',
                    style: TextStyle(fontSize: 11, color: Cozy.dimGray, letterSpacing: 1)),
                const SizedBox(height: 4),
                Text('tap [ENROLL] to add one via rule builder or .bin paste',
                    style: TextStyle(fontSize: 10, color: Cozy.dimGray)),
              ]),
            ),
          );
        }
        if (_selected >= models.length) _selected = models.length - 1;
        return ValueListenableBuilder<String?>(
          valueListenable: widget.bleService.activeModel,
          builder: (context, active, __) => SizedBox(
            height: 165,
            child: PageView.builder(
              controller: PageController(viewportFraction: 0.84, initialPage: _selected),
              itemCount: models.length,
              onPageChanged: (i) => setState(() => _selected = i),
              itemBuilder: (context, i) {
                final name = models[i];
                final isActive =
                    active != null && active.toLowerCase() == name.toLowerCase();
                final isFocused = _selected == i;
                return Transform.scale(
                  scale: isFocused ? 1.0 : 0.94,
                  child: Opacity(
                    opacity: isFocused ? 1.0 : 0.5,
                    child: GestureDetector(
                      onTap: () => _activateModel(name),
                      onLongPress: () => _modelActions(name),
                      child: FrostedBox(
                        padding: const EdgeInsets.all(18),
                        borderRadius: BorderRadius.circular(26),
                        backgroundColor: isActive
                            ? _accent.withValues(alpha: 0.10)
                            : Colors.white.withValues(alpha: 0.035),
                        borderColor: isActive
                            ? _accent.withValues(alpha: 0.45)
                            : Colors.white.withValues(alpha: 0.07),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          mainAxisAlignment: MainAxisAlignment.spaceBetween,
                          children: [
                            Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
                              Text('MDL-${i.toString().padLeft(2, '0')}',
                                  style: TextStyle(fontSize: 10, color: Cozy.dimGray, letterSpacing: 1.2)),
                              GestureDetector(
                                onTap: () => _deleteModel(name),
                                child: Container(
                                  padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                                  decoration: BoxDecoration(
                                    color: Colors.white.withValues(alpha: 0.06),
                                    borderRadius: BorderRadius.circular(6),
                                  ),
                                  child: const Text('DELETE',
                                      style: TextStyle(color: Cozy.roseError, fontSize: 9, fontWeight: FontWeight.bold)),
                                ),
                              ),
                            ]),
                            Row(children: [
                              Icon(isActive ? Icons.verified_rounded : Icons.local_florist_rounded,
                                  color: _accent, size: 24),
                              const SizedBox(width: 10),
                              Expanded(
                                child: Text(name.toUpperCase(),
                                    style: const TextStyle(fontSize: 15, fontWeight: FontWeight.bold),
                                    overflow: TextOverflow.ellipsis),
                              ),
                            ]),
                            Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
                              Text(isActive ? 'ACTIVE · LOADED IN RAM' : 'TAP TO ACTIVATE',
                                  style: TextStyle(
                                      fontSize: 10.5,
                                      fontWeight: FontWeight.bold,
                                      letterSpacing: 0.8,
                                      color: isActive ? _accent : Cozy.dimGray)),
                              const Text('Fruit28D',
                                  style: TextStyle(fontSize: 10, color: Cozy.dimGray)),
                            ]),
                          ],
                        ),
                      ),
                    ),
                  ),
                );
              },
            ),
          ),
        );
      },
    );
  }

  /// Mirrors the device LCD flow: SCAN → hold to camera → PLACE ON PIEZO →
  /// SCAN again (device button OR this button) → TAP NOW → result.
  Widget _scanButton() {
    final confirming = _phase == 'place_on_piezo';
    final armed = _phase == 'acoustic_armed';

    return ValueListenableBuilder<bool>(
      valueListenable: widget.bleService.connected,
      builder: (_, connected, __) {
        final label = !connected
            ? 'CONNECT SCANNER FIRST'
            : confirming
                ? 'STEP 2 · CONFIRM PLACEMENT & ARM TAP'
                : armed
                    ? 'TAP THE FRUIT NOW — LISTENING'
                    : _running
                        ? 'ABORT SESSION'
                        : 'ACQUIRE SPECTRAL SAMPLE';
        final fg = !connected || _running || armed ? Cozy.oatmeal : Cozy.deepBg;
        final bg = !connected
            ? Colors.white.withValues(alpha: 0.05)
            : (confirming || armed)
                ? _accent.withValues(alpha: 0.25)
                : _running
                    ? Colors.white.withValues(alpha: 0.08)
                    : _accent.withValues(alpha: 0.85);

        return GestureDetector(
          onTap: !connected
              ? null
              : confirming
                  ? () => widget.bleService.startInference() // stage 2
                  : armed
                      ? null // waiting for the physical tap
                      : (_running ? _cancelScan : _startScan),
          child: AnimatedContainer(
            duration: const Duration(milliseconds: 200),
            width: double.infinity,
            padding: const EdgeInsets.symmetric(vertical: 16),
            decoration: BoxDecoration(
              color: bg,
              borderRadius: BorderRadius.circular(18),
              border: confirming || armed
                  ? Border.all(color: _accent.withValues(alpha: 0.6))
                  : null,
              boxShadow: (!connected || _running || confirming || armed)
                  ? []
                  : [BoxShadow(color: _accent.withValues(alpha: 0.2), blurRadius: 16, offset: const Offset(0, 4))],
            ),
            child: Row(mainAxisAlignment: MainAxisAlignment.center, children: [
              Icon(!connected
                      ? Icons.link_off_rounded
                      : confirming
                          ? Icons.touch_app_rounded
                          : armed
                              ? Icons.graphic_eq_rounded
                              : _running
                                  ? Icons.close_rounded
                                  : Icons.filter_vintage_rounded,
                  color: fg, size: 20),
              const SizedBox(width: 10),
              Text(label,
                  style: TextStyle(
                      color: fg,
                      fontSize: 12,
                      fontWeight: FontWeight.bold,
                      letterSpacing: 0.8)),
            ]),
          ),
        );
      },
    );
  }

  // ── Real inference readout in frosted pills ────────────────────────
  Widget _realResultCard(ScanResultData r) {
    String pct(double v) =>
        '${v.clamp(0, 100).toStringAsFixed(1)}%'; // fw sends 0-100
    final accentColor = switch (r.decision) {
      'PERFECTLY_RIPE' => Cozy.matcha,
      'OVERRIPE' => Cozy.chamomile,
      'ROTTEN_OR_HOLLOW' => Cozy.roseError,
      'ARTIFICIALLY_RIPENED' => Cozy.heatherPink,
      _ => Cozy.duskBlue, // UNRIPE / unknown
    };

    return FrostedBox(
      borderColor: accentColor.withValues(alpha: 0.35),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
          const SectionLabel(title: '// ANALYSIS READOUT'),
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
            decoration: BoxDecoration(
                color: Colors.white.withValues(alpha: 0.06),
                borderRadius: BorderRadius.circular(6)),
            child: Text(r.decision,
                style: TextStyle(fontSize: 10, fontWeight: FontWeight.bold, color: accentColor)),
          ),
        ]),
        const SizedBox(height: 16),
        MetricPill(title: 'PRIMARY_DECISION', value: r.decision.replaceAll('_', ' '), highlight: accentColor),
        const SizedBox(height: 8),
        MetricPill(title: 'CONFIDENCE', value: pct(r.confidence), highlight: Cozy.matcha),
        const SizedBox(height: 8),
        MetricPill(title: 'RIPENESS_INDEX', value: r.ripenessIndex.toStringAsFixed(3), highlight: Cozy.chamomile),
        const SizedBox(height: 8),
        MetricPill(title: 'TRANSITION_ENTROPY', value: r.entropy.toStringAsFixed(3), highlight: Cozy.linenAlmond),
        const SizedBox(height: 8),
        MetricPill(
            title: 'ANOMALY_FLAG',
            value: r.isAnomaly ? 'DETECTED ⚠' : 'CLEAR',
            highlight: r.isAnomaly ? Cozy.roseError : Cozy.matcha),
        const SizedBox(height: 14),
        for (final e in r.probabilities.entries)
          Padding(
            padding: const EdgeInsets.only(bottom: 6),
            child: Row(children: [
              SizedBox(
                width: 150,
                child: Text(e.key.toLowerCase().replaceAll('_', ' '),
                    style: TextStyle(fontSize: 10, color: Cozy.dimGray)),
              ),
              Expanded(
                child: ClipRRect(
                  borderRadius: BorderRadius.circular(4),
                  child: TweenAnimationBuilder<double>(
                    tween: Tween(begin: 0, end: e.value.clamp(0, 1)),
                    duration: const Duration(milliseconds: 700),
                    curve: Curves.easeOutCubic,
                    builder: (_, v, __) => LinearProgressIndicator(
                        value: v,
                        minHeight: 5,
                        backgroundColor: Colors.white.withValues(alpha: 0.05),
                        valueColor: AlwaysStoppedAnimation<Color>(accentColor.withValues(alpha: 0.85))),
                  ),
                ),
              ),
              SizedBox(
                width: 42,
                child: Text('${(e.value * 100).toStringAsFixed(0)}%',
                    textAlign: TextAlign.right,
                    style: TextStyle(fontSize: 10, color: Cozy.warmGray)),
              ),
            ]),
          ),
      ]),
    );
  }
}
