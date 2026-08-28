import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/cozy_palette.dart';
import '../core/rules_model.dart';
import '../core/training_repository.dart';
import '../services/ble_service.dart';
import '../widgets/frosted.dart';
import '../widgets/spectra_charts.dart';
import 'data_collection_screen.dart';

typedef Knobs = Map<String, double>;

/// Human rule-space builder: per-class physical knobs -> 616-byte model.
/// Mirrors the Rules Builder tab of collectorrrr.py / rules_engine.py.
/// Live hue-histogram + tap-spectrum previews update while dragging.
class RuleBuilderScreen extends StatefulWidget {
  final BleService bleService;
  const RuleBuilderScreen({super.key, required this.bleService});

  @override
  State<RuleBuilderScreen> createState() => _RuleBuilderScreenState();
}

class _RuleBuilderScreenState extends State<RuleBuilderScreen>
    with SingleTickerProviderStateMixin {
  final _nameCtrl = TextEditingController(text: 'Mango');
  final _repo = TrainingRepository.instance;
  late final Set<String> _enabled;
  late final Map<String, Knobs> _knobs;
  double _temp = 2.0; // sharpness MULTIPLIER on the auto-calibrated temp
  String? _previewClass;
  final Set<String> _measured = {'UNRIPE', 'PERFECTLY_RIPE'};

  // floating preview: pops while a knob of this class is being dragged
  String? _focusClass;
  Timer? _hideTimer;
  void _focusPreview(String label) {
    _hideTimer?.cancel();
    if (_focusClass != label) setState(() => _focusClass = label);
  }
  void _blurPreview() {
    _hideTimer?.cancel();
    _hideTimer = Timer(const Duration(milliseconds: 900), () {
      if (mounted) setState(() => _focusClass = null);
    });
  }

  @override
  void initState() {
    super.initState();
    _enabled = {'UNRIPE', 'PERFECTLY_RIPE'};
    _knobs = {
      for (final e in RulesModel.presets.entries) e.key: Map.of(e.value),
    };
    _repo.addListener(_onRepoChanged);
  }

  void _onRepoChanged() {
    if (mounted) setState(() {});
  }

  @override
  void dispose() {
    _repo.removeListener(_onRepoChanged);
    super.dispose();
  }

  Color _classColor(String label) =>
      Cozy.accents[RulesModel.classLabels.indexOf(label) % Cozy.accents.length];

  // ── Derived ────────────────────────────────────────────────────────
  /// Prototype for a class: connected measured data centroid when present,
  /// else the built-in MEASURED prototype (when that source is picked), else
  /// knob-generated.
  List<double> _protoOf(String label) {
    final collected = _repo.prototypeOf(label);
    if (collected != null) return collected;
    if (_measured.contains(label)) {
      final m = RulesModel.measuredPrototypes[label];
      if (m != null) return m;
    }
    return RulesModel.stateFromKnobs(_knobs[label]!);
  }

  ({List<List<double>> w, List<double> b, int mask, double temp})? get _built {
    if (_enabled.isEmpty) return null;
    final protos = {for (final l in _enabled) l: _protoOf(l)};
    return RulesModel.buildFromPrototypes(protos, _enabled.toList(),
        tempFactor: _temp);
  }

  bool get _selftestOk {
    final b = _built;
    if (b == null) return false;
    final protos = {for (final l in _enabled) l: _protoOf(l)};
    return RulesModel.selftest(protos, b.w, b.b, b.mask);
  }

  // ── Actions ────────────────────────────────────────────────────────
  Future<void> _buildAndUpload() async {
    final built = _built;
    if (built == null || !_selftestOk) {
      _toast('Rules are inconsistent — selftest failed');
      return;
    }
    final name = _nameCtrl.text.trim();
    if (name.isEmpty) {
      _toast('Give the fruit a name');
      return;
    }
    final bin = RulesModel.packBinary(name, built.w, built.b, built.mask);

    if (!widget.bleService.isConnected) {
      await _exportToClipboard(bin);
      return;
    }

    final notifier = ValueNotifier(0.0);
    showDialog<void>(
      context: context,
      barrierDismissible: false,
      builder: (ctx) => PopScope(
        canPop: false,
        child: AlertDialog(
          backgroundColor: Cozy.surfaceCard,
          title: Text('UPLOADING "${name.toUpperCase()}"',
              style: const TextStyle(
                  fontFamily: Cozy.monoFamily, fontSize: 14, color: Cozy.matcha)),
          content: Column(mainAxisSize: MainAxisSize.min, children: [
            ValueListenableBuilder<double>(
              valueListenable: notifier,
              builder: (_, v, __) => LinearProgressIndicator(
                  value: v > 0 ? v : null, minHeight: 8),
            ),
            const SizedBox(height: 12),
            Text('streaming to ESP32 flash…',
                style: TextStyle(fontSize: 11, color: Cozy.warmGray)),
          ]),
        ),
      ),
    );

    final ok = await widget.bleService.uploadModelBin(bin,
        onProgress: (v) => notifier.value = v);

    final nav = Navigator.of(context, rootNavigator: true);
    if (nav.canPop()) nav.pop(); // close progress dialog
    if (!mounted) return;
    _toast(ok ? 'Saved to device flash!' : 'Upload failed');
  }

  Future<void> _exportToClipboard(Uint8List bin) async {
    await Clipboard.setData(ClipboardData(text: RulesModel.toBase64(bin)));
    _toast('No device connected — base64 .bin copied to clipboard');
  }

  /// Open the live data-collection screen for a category.
  Future<void> _connectData(String label) async {
    final fruit = _nameCtrl.text.trim().isEmpty ? 'Mango' : _nameCtrl.text.trim();
    final saved = await Navigator.push<bool>(
      context,
      MaterialPageRoute(
        builder: (_) => DataCollectionScreen(
          bleService: widget.bleService,
          category: label,
          fruitName: fruit,
        ),
      ),
    );
    if (saved ?? false) {
      _toast('Recorded ${_repo.sessionsFor(label).length} session(s) for $label');
    }
  }

  /// Reopen an already-recorded session in the collection window so the user
  /// can review, delete taps/hues, rename and re-save it.
  Future<void> _editData(String label, SampleSession s) async {
    final fruit = s.fruit;
    final saved = await Navigator.push<bool>(
      context,
      MaterialPageRoute(
        builder: (_) => DataCollectionScreen(
          bleService: widget.bleService,
          category: s.category,
          fruitName: fruit,
          editSession: s,
        ),
      ),
    );
    if (saved ?? false) {
      _toast('Updated session for ${s.category.replaceAll('_', ' ')}');
    }
  }

  /// Pull previously-saved sessions back from disk (survives an app restart).
  Future<void> _loadFromDisk() async {
    if (Platform.isLinux && _repo.pickedBaseDir == null) {
      final ok = await _repo.pickBaseDir();
      if (!ok) return;
    }
    final n = await _repo.loadFromDisk();
    _toast(n == 0 ? 'No new saved sessions found on disk' : 'Loaded $n saved session(s)');
  }

  /// Train ALL categories that have connected data into one model.
  Future<void> _trainAll() async {
    final name = _nameCtrl.text.trim();
    if (name.isEmpty) {
      _toast('Give the fruit a name');
      return;
    }
    final bin = _repo.trainAll(
        fruitName: name, enabledLabels: _enabled.toList(), tempFactor: _temp);
    if (bin == null) {
      _toast('Connect data to at least two enabled categories to train');
      return;
    }
    if (!widget.bleService.isConnected) {
      await _exportToClipboard(bin);
      return;
    }
    final ok = await widget.bleService.uploadModelBin(bin);
    _toast(ok ? 'Trained & saved to device flash!' : 'Upload failed');
  }

  void _toast(String msg) {
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      behavior: SnackBarBehavior.floating,
      duration: const Duration(seconds: 3),
      content: Text(msg),
    ));
  }

  // ── Build ──────────────────────────────────────────────────────────
  @override
  Widget build(BuildContext context) {
    final ok = _selftestOk;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Rule Builder'),
        actions: [
          IconButton(
            tooltip: 'Load saved sessions from disk',
            icon: const Icon(Icons.folder_open_rounded),
            onPressed: _loadFromDisk,
          ),
          IconButton(
            tooltip: 'Copy .bin as base64',
            icon: const Icon(Icons.copy_all_rounded),
            onPressed: () {
              final b = _built;
              if (b == null || _nameCtrl.text.trim().isEmpty) {
                _toast('Name it and enable a class first');
                return;
              }
              _exportToClipboard(RulesModel.packBinary(
                  _nameCtrl.text.trim(), b.w, b.b, b.mask));
            },
          ),
        ],
      ),
      body: SafeArea(
        child: Stack(children: [
          LayoutBuilder(builder: (context, cons) {
          final wide = cons.maxWidth > 920;

          final controls = <Widget>[
            TextField(
              controller: _nameCtrl,
              maxLength: 31,
              style: const TextStyle(fontFamily: Cozy.monoFamily, fontSize: 13),
              decoration: InputDecoration(
                labelText: 'FRUIT NAME',
                labelStyle: TextStyle(fontSize: 11, color: Cozy.dimGray),
                prefixIcon: const Icon(Icons.local_florist_rounded,
                    color: Cozy.matcha),
                filled: true,
                fillColor: Colors.white.withValues(alpha: 0.03),
                border: OutlineInputBorder(
                    borderRadius: BorderRadius.circular(16),
                    borderSide:
                        BorderSide(color: Colors.white.withValues(alpha: 0.08))),
                focusedBorder: OutlineInputBorder(
                    borderRadius: BorderRadius.circular(16),
                    borderSide: const BorderSide(color: Cozy.matcha)),
              ),
            ),
            const SizedBox(height: 10),

            FrostedBox(
              padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
              borderRadius: BorderRadius.circular(18),
              backgroundColor: ok
                  ? Cozy.matcha.withValues(alpha: 0.08)
                  : Cozy.roseError.withValues(alpha: 0.08),
              borderColor: ok
                  ? Cozy.matcha.withValues(alpha: 0.3)
                  : Cozy.roseError.withValues(alpha: 0.3),
              child: Row(children: [
                Icon(ok ? Icons.verified_rounded : Icons.warning_amber_rounded,
                    size: 17,
                    color: ok ? Cozy.matcha : Cozy.roseError),
                const SizedBox(width: 10),
                Expanded(
                  child: Text(
                    ok
                        ? 'SELFTEST OK — prototypes classify as themselves'
                        : _enabled.isEmpty
                            ? 'ENABLE AT LEAST ONE CLASS'
                            : 'SELFTEST FAILED',
                    style: TextStyle(
                        fontSize: 10.5,
                        letterSpacing: 0.6,
                        fontWeight: FontWeight.bold,
                        color: ok ? Cozy.matcha : Cozy.roseError),
                  ),
                ),
              ]),
            ),
            const SizedBox(height: 6),

            _SliderTile(
              label: _built == null
                  ? 'sharpness multiplier'
                  : 'sharpness multiplier · temp ${_built!.temp.toStringAsFixed(1)}',
              hint: 'auto-calibrated to ~90% at prototypes; <1 sharper, >1 softer',
              value: _temp,
              min: 0.25,
              max: 4.0,
              divisions: 15,
              format: (v) => '${v.toStringAsFixed(2)}x',
              color: Cozy.linenAlmond,
              onChanged: (v) => setState(() => _temp = v),
            ),

            for (final label in RulesModel.classLabels)
              _ClassCard(
                label: label,
                color: _classColor(label),
                knobs: _knobs[label]!,
                enabled: _enabled.contains(label),
                measured: _measured.contains(label) &&
                    RulesModel.measuredPrototypes.containsKey(label),
                sessions: _repo.sessionsFor(label),
                onConnect: () => _connectData(label),
                onRemoveSession: (id) =>
                    setState(() => _repo.removeSession(label, id)),
                onEditSession: (s) => _editData(label, s),
                onToggleSource: (m) => setState(() =>
                    m ? _measured.add(label) : _measured.remove(label)),
                onChanged: (on) => setState(
                    () => on ? _enabled.add(label) : _enabled.remove(label)),
                onKnob: (k, v) => setState(() => _knobs[label]![k] = v),
                onFocus: _focusPreview,
                onBlur: _blurPreview,
              ),

            const SizedBox(height: 16),
            if (_repo.connectedCategories.isNotEmpty)
              OutlinedButton.icon(
                style: OutlinedButton.styleFrom(
                  minimumSize: const Size.fromHeight(48),
                  foregroundColor: Cozy.duskBlue,
                  side: BorderSide(
                      color: Cozy.duskBlue.withValues(alpha: 0.5)),
                  shape: RoundedRectangleBorder(
                      borderRadius: BorderRadius.circular(18)),
                ),
                onPressed: _enabled.isEmpty ? null : _trainAll,
                icon: const Icon(Icons.science_rounded, size: 19),
                label: Text(
                  'TRAIN ${_repo.connectedCategories.length} CONNECTED CATEGOR${_repo.connectedCategories.length == 1 ? 'Y' : 'IES'}',
                  style:
                      const TextStyle(fontWeight: FontWeight.bold, letterSpacing: 1),
                ),
              ),
            const SizedBox(height: 10),
            FilledButton.icon(
              style: FilledButton.styleFrom(
                minimumSize: const Size.fromHeight(54),
                backgroundColor: Cozy.matcha,
                foregroundColor: Cozy.deepBg,
                shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(18)),
              ),
              onPressed: _enabled.isEmpty ? null : _buildAndUpload,
              icon: const Icon(Icons.upload_rounded),
              label: Text(
                widget.bleService.isConnected ? 'BUILD & UPLOAD' : 'BUILD & EXPORT',
                style: const TextStyle(fontWeight: FontWeight.bold, letterSpacing: 1),
              ),
            ),
            const SizedBox(height: 24),
          ];

          final preview = _previewPanel();

          if (wide) {
            // two-pane: controls left, always-visible spectra right
            return Row(children: [
              Expanded(
                flex: 5,
                child: ListView(padding: const EdgeInsets.all(16), children: controls),
              ),
              VerticalDivider(width: 1, color: Colors.white.withValues(alpha: 0.07)),
              Expanded(flex: 6, child: preview),
            ]);
          }
          // phone: preview pinned above the knob cards so drags stay visible
          return ListView(
            padding: const EdgeInsets.fromLTRB(16, 12, 16, 24),
            children: [
              _previewContent(),
              const SizedBox(height: 16),
              ...controls,
            ],
          );
          }),
          // ── FLOATING LIVE PREVIEW (drag-only) ──
          if (_focusClass != null)
            Positioned(
              left: 14, right: 14, bottom: 14,
              child: IgnorePointer(
                child: AnimatedOpacity(
                  opacity: 1,
                  duration: const Duration(milliseconds: 150),
                  child: FrostedBox(
                    padding: const EdgeInsets.all(12),
                    borderRadius: BorderRadius.circular(20),
                    backgroundColor:
                        Cozy.surfaceCard.withValues(alpha: 0.96),
                    borderColor:
                        _classColor(_focusClass!).withValues(alpha: 0.55),
                    child: Column(mainAxisSize: MainAxisSize.min,
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Row(children: [
                          Container(width: 8, height: 8,
                              decoration: BoxDecoration(
                                  color: _classColor(_focusClass!),
                                  shape: BoxShape.circle)),
                          const SizedBox(width: 6),
                          Text(
                              _focusClass!.toLowerCase().replaceAll('_', ' '),
                              style: TextStyle(
                                  fontFamily: Cozy.monoFamily,
                                  fontSize: 10.5,
                                  fontWeight: FontWeight.bold,
                                  letterSpacing: 1,
                                  color: _classColor(_focusClass!))),
                          const Spacer(),
                          Text('LIVE',
                              style: TextStyle(
                                  fontSize: 9,
                                  letterSpacing: 2,
                                  color: Cozy.dimGray)),
                        ]),
                        const SizedBox(height: 8),
                        SizedBox(
                          height: 58,
                          child: HueHistogramChart(
                            series: PlotSeries(
                                _focusClass!,
                                _classColor(_focusClass!),
                                RulesModel.stateFromKnobs(
                                        _knobs[_focusClass]!)
                                    .sublist(0, 8))),
                        ),
                        const SizedBox(height: 6),
                        SizedBox(
                          height: 58,
                          child: SpectrumChart(
                            series: PlotSeries(
                                _focusClass!,
                                _classColor(_focusClass!),
                                RulesModel.stateFromKnobs(
                                        _knobs[_focusClass]!)
                                    .sublist(10, 25))),
                        ),
                      ]),
                  ),
                ),
              ),
            ),
        ]),
      ),
    );
  }

  // ── LIVE SPECTRA PANEL ─────────────────────────────────────────────
  String? get _effectivePreviewClass {
    if (_previewClass != null && _enabled.contains(_previewClass)) {
      return _previewClass;
    }
    return _enabled.isEmpty ? null : _enabled.first;
  }

  /// Scrollable pane for the wide two-column layout.
  Widget _previewPanel() {
    return ListView(padding: const EdgeInsets.all(16), children: [
      _previewContent(),
    ]);
  }

  /// Inline (non-scrollable) content shared by both layouts.
  Widget _previewContent() {
    final shown = _effectivePreviewClass;

    PlotSeries? hueSeries;
    PlotSeries? specSeries;
    if (shown != null) {
      final st = _protoOf(shown);
      final c = _classColor(shown);
      hueSeries = PlotSeries(shown, c, st.sublist(0, 8));
      specSeries = PlotSeries(shown, c, st.sublist(10, 25));
    }

    return FrostedBox(
      borderRadius: BorderRadius.circular(22),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          const Expanded(child: SectionLabel(title: '// HUE SIGNATURE · LIVE')),
          Flexible(
            child: Text(
                shown == null ? '—' : shown.replaceAll('_', ' ').toLowerCase(),
                overflow: TextOverflow.ellipsis,
                style: TextStyle(
                    fontSize: 10,
                    fontWeight: FontWeight.bold,
                    color:
                        shown == null ? Cozy.dimGray : _classColor(shown))),
          ),
        ]),
        const SizedBox(height: 8),

        // one-class-at-a-time selector
        Wrap(
          spacing: 6,
          runSpacing: 4,
          children: [
            for (final label in RulesModel.classLabels.where(_enabled.contains))
              GestureDetector(
                onTap: () => setState(() => _previewClass = label),
                child: Container(
                  padding:
                      const EdgeInsets.symmetric(horizontal: 9, vertical: 4),
                  decoration: BoxDecoration(
                    color: label == shown
                        ? _classColor(label).withValues(alpha: 0.18)
                        : Colors.white.withValues(alpha: 0.04),
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(
                        color: label == shown
                            ? _classColor(label).withValues(alpha: 0.7)
                            : Colors.white.withValues(alpha: 0.08)),
                  ),
                  child: Text(label.toLowerCase().replaceAll('_', ' '),
                      style: TextStyle(
                          fontSize: 9.5,
                          fontWeight: FontWeight.bold,
                          color: label == shown
                              ? _classColor(label)
                              : Cozy.warmGray)),
                ),
              ),
          ],
        ),
        const SizedBox(height: 10),

        SizedBox(
            height: 140,
            child: HueHistogramChart(series: hueSeries)),
        const SizedBox(height: 18),
        const SectionLabel(title: '// TAP SPECTRUM · LIVE'),
        Text('conditioned log-power @ band centres',
            style: TextStyle(fontSize: 9, color: Cozy.dimGray)),
        const SizedBox(height: 8),
        SizedBox(height: 150, child: SpectrumChart(series: specSeries)),
        const SizedBox(height: 6),
        Text(
            'drag skin_hue / spread to reshape the bars · firmness slides the '
            'spectrum peak 1 kHz → 300 Hz · character widens it',
            softWrap: true,
            style:
                TextStyle(fontSize: 9.5, height: 1.5, color: Cozy.dimGray)),
      ]),
    );
  }
}

// ── Per-class card with the four physical knobs ──────────────────────
class _ClassCard extends StatelessWidget {
  final String label;
  final Color color;
  final Knobs knobs;
  final bool enabled;
  final bool measured;
  final List<SampleSession> sessions;
  final VoidCallback onConnect;
  final ValueChanged<String> onRemoveSession;
  final ValueChanged<SampleSession> onEditSession;
  final ValueChanged<bool> onToggleSource;
  final ValueChanged<bool> onChanged;
  final void Function(String key, double v) onKnob;
  final ValueChanged<String> onFocus;
  final VoidCallback onBlur;

  const _ClassCard({
    required this.label,
    required this.color,
    required this.knobs,
    required this.enabled,
    required this.measured,
    required this.sessions,
    required this.onConnect,
    required this.onRemoveSession,
    required this.onEditSession,
    required this.onToggleSource,
    required this.onChanged,
    required this.onKnob,
    required this.onFocus,
    required this.onBlur,
  });

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final hue = knobs['skin_hue_deg'] ?? 60;
    return Card(
      margin: const EdgeInsets.only(top: 10),
      elevation: 0,
      color: enabled ? cs.surfaceContainer : cs.surfaceContainerLowest,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(20),
        side: BorderSide(
            color: enabled
                ? color.withValues(alpha: 0.45)
                : Colors.white.withValues(alpha: 0.05),
            width: 1.2),
      ),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(14, 4, 14, 12),
        child: Column(children: [
          SwitchListTile.adaptive(
            contentPadding: EdgeInsets.zero,
            dense: true,
            value: enabled,
            activeThumbColor: color,
            activeColor: color,
            secondary: Container(
              width: 26,
              height: 26,
              decoration: BoxDecoration(
                color: HSVColor.fromAHSV(1, hue.clamp(20, 120), .85, 1).toColor(),
                borderRadius: BorderRadius.circular(9),
              ),
            ),
            title: Text(label.replaceAll('_', ' '),
                style: TextStyle(
                    fontFamily: Cozy.monoFamily,
                    fontSize: 12,
                    fontWeight: FontWeight.w800,
                    letterSpacing: 1,
                    color: enabled ? cs.onSurface : cs.outline)),
            onChanged: onChanged,
          ),
          if (enabled)
            Padding(
              padding: const EdgeInsets.only(bottom: 6),
              child: Row(children: [
                _sourceChip('KNOBS', !measured, color, cs,
                    () => onToggleSource(false)),
                const SizedBox(width: 6),
                _sourceChip('MEASURED', measured, color, cs,
                    () => onToggleSource(true)),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                      measured
                          ? 'prototype = recorded data centroid'
                          : 'prototype = knob-generated',
                      style: TextStyle(fontSize: 9, color: Cozy.dimGray),
                      overflow: TextOverflow.ellipsis),
                ),
              ]),
            ),
          AnimatedSwitcher(
            duration: const Duration(milliseconds: 250),
            child: !enabled
                ? const SizedBox(width: double.infinity)
                : measured
                    ? Padding(
                        padding: const EdgeInsets.only(bottom: 8),
                        child: Align(
                          alignment: Alignment.centerLeft,
                          child: Text(
                              'using measured prototype from '
                              'RulesModel.measuredPrototypes — '
                              'switch to KNOBS to fine-tune',
                              style: TextStyle(
                                  fontSize: 9.5,
                                  height: 1.4,
                                  color: Cozy.warmGray)),
                        ),
                      )
                    : Column(
                    key: ValueKey(enabled),
                    children: [
                      _SliderTile(
                        label: 'skin hue',
                        value: hue,
                        min: RulesModel.knobLimits['skin_hue_deg']!.$1,
                        max: RulesModel.knobLimits['skin_hue_deg']!.$2,
                        format: (v) => '${v.toStringAsFixed(0)}°',
                        color:
                            HSVColor.fromAHSV(1, hue.clamp(20, 120), .9, 1).toColor(),
                        onChanged: (v) => onKnob('skin_hue_deg', v),
                          onDragStart: () => onFocus(label),
                          onDragEnd: () => onBlur(),
                      ),
                      _SliderTile(
                        label: 'spread',
                        hint: 'blotchiness of colouring',
                        value: knobs['spread_deg'] ?? 15,
                        min: RulesModel.knobLimits['spread_deg']!.$1,
                        max: RulesModel.knobLimits['spread_deg']!.$2,
                        format: (v) => '${v.toStringAsFixed(0)}°',
                        color: color.withValues(alpha: 0.75),
                        onChanged: (v) => onKnob('spread_deg', v),
                          onDragStart: () => onFocus(label),
                          onDragEnd: () => onBlur(),
                      ),
                      _SliderTile(
                        label: 'firmness',
                        hint: 'hard ping → soft thud',
                        value: knobs['firmness'] ?? 0.5,
                        min: 0,
                        max: 1,
                        format: (v) => v.toStringAsFixed(2),
                        color: Cozy.chamomile,
                        onChanged: (v) => onKnob('firmness', v),
                          onDragStart: () => onFocus(label),
                          onDragEnd: () => onBlur(),
                      ),
                      _SliderTile(
                        label: 'character',
                        hint: 'crisp resonant → dull mushy',
                        value: knobs['character'] ?? 0.5,
                        min: 0,
                        max: 1,
                        format: (v) => v.toStringAsFixed(2),
                        color: Cozy.heatherPink,
                        onChanged: (v) => onKnob('character', v),
                          onDragStart: () => onFocus(label),
                          onDragEnd: () => onBlur(),
                      ),
                    ],
                  ),
          ),
          const SizedBox(height: 8),
          Divider(height: 1, color: Colors.white.withValues(alpha: 0.07)),
          const SizedBox(height: 8),
          // ── CONNECTED DATA section ──
          Row(children: [
            Icon(Icons.storage_rounded, size: 15, color: color),
            const SizedBox(width: 8),
            Expanded(
              child: Text(
                sessions.isEmpty
                    ? 'NO MEASURED DATA CONNECTED'
                    : '${sessions.length} SESSION${sessions.length == 1 ? '' : 'S'} · '
                        '${sessions.fold<int>(0, (a, s) => a + s.tapCount)} TAPS',
                style: TextStyle(
                    fontSize: 9.5,
                    fontWeight: FontWeight.bold,
                    letterSpacing: 0.8,
                    color: sessions.isEmpty ? Cozy.dimGray : color),
              ),
            ),
            TextButton.icon(
              style: TextButton.styleFrom(foregroundColor: color),
              onPressed: enabled ? onConnect : null,
              icon: const Icon(Icons.add_link_rounded, size: 16),
              label: const Text('CONNECT DATA',
                  style: TextStyle(fontWeight: FontWeight.bold, fontSize: 10)),
            ),
          ]),
          if (sessions.isNotEmpty)
            Padding(
              padding: const EdgeInsets.only(top: 4),
              child: Column(children: [
                for (final s in sessions)
                  Container(
                    margin: const EdgeInsets.only(bottom: 4),
                    padding:
                        const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                    decoration: BoxDecoration(
                      color: Colors.white.withValues(alpha: 0.03),
                      borderRadius: BorderRadius.circular(8),
                    ),
                    child: Row(children: [
                      Expanded(
                        child: Text(
                          '${s.id} · ${s.tapCount} taps',
                          style: const TextStyle(
                              fontSize: 9.5,
                              fontFamily: Cozy.monoFamily,
                              color: Cozy.warmGray),
                          overflow: TextOverflow.ellipsis,
                        ),
                      ),
                      IconButton(
                        visualDensity: VisualDensity.compact,
                        padding: EdgeInsets.zero,
                        icon: const Icon(Icons.edit_outlined, size: 15,
                            color: Cozy.duskBlue),
                        tooltip: 'Reopen & edit this session',
                        onPressed: () => onEditSession(s),
                      ),
                      IconButton(
                        visualDensity: VisualDensity.compact,
                        padding: EdgeInsets.zero,
                        icon: const Icon(Icons.remove_circle_outline_rounded,
                            size: 15, color: Cozy.roseError),
                        tooltip: 'Remove this session',
                        onPressed: () => onRemoveSession(s.id),
                      ),
                    ]),
                  ),
              ]),
            ),
        ]),
      ),
    );
  }
}

class _SliderTile extends StatelessWidget {
  final String label;
  final String? hint;
  final double value, min, max;
  final int? divisions;
  final String Function(double) format;
  final Color color;
  final ValueChanged<double> onChanged;
  final VoidCallback? onDragStart;
  final VoidCallback? onDragEnd;

  const _SliderTile({
    required this.label,
    this.hint,
    required this.value,
    required this.min,
    required this.max,
    required this.format,
    required this.color,
    required this.onChanged,
    this.divisions,
    this.onDragStart,
    this.onDragEnd,
  });

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(
        children: [
          Expanded(
            flex: 3,
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('$label  ·  ${format(value)}',
                    style: TextStyle(
                        fontSize: 11,
                        fontWeight: FontWeight.w700,
                        letterSpacing: 0.6,
                        color: cs.onSurfaceVariant)),
                if (hint != null)
                  Text(hint!,
                      style: TextStyle(fontSize: 9.5, color: cs.outline)),
              ],
            ),
          ),
          Expanded(
            flex: 7,
            child: SliderTheme(
              data: SliderTheme.of(context).copyWith(
                trackHeight: 5,
                thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 8),
                overlayShape: const RoundSliderOverlayShape(overlayRadius: 14),
                activeTrackColor: color.withValues(alpha: 0.85),
                inactiveTrackColor: cs.surfaceContainerHighest,
                thumbColor: color,
                overlayColor: color.withValues(alpha: 0.15),
              ),
              child: Slider(
                value: value.clamp(min, max),
                min: min,
                max: max,
                divisions: divisions,
                onChanged: onChanged,
                onChangeStart: onDragStart == null ? null : (_) => onDragStart!(),
                onChangeEnd: onDragEnd == null ? null : (_) => onDragEnd!(),
              ),
            ),
          ),
        ],
      ),
    );
  }
}


class _sourceChip extends StatelessWidget {
  final String text;
  final bool selected;
  final Color color;
  final ColorScheme cs;
  final VoidCallback onTap;

  const _sourceChip(this.text, this.selected, this.color, this.cs, this.onTap);

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onTap: onTap,
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 4),
        decoration: BoxDecoration(
          color: selected
              ? color.withValues(alpha: 0.18)
              : Colors.white.withValues(alpha: 0.04),
          borderRadius: BorderRadius.circular(8),
          border: Border.all(
              color: selected
                  ? color.withValues(alpha: 0.7)
                  : Colors.white.withValues(alpha: 0.08)),
        ),
        child: Text(text,
            style: TextStyle(
                fontFamily: Cozy.monoFamily,
                fontSize: 9.5,
                fontWeight: FontWeight.bold,
                letterSpacing: 1,
                color: selected ? color : Cozy.warmGray)),
      ),
    );
  }
}
