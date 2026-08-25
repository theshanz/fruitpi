import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/cozy_palette.dart';
import '../core/rules_model.dart';
import '../services/ble_service.dart';
import '../widgets/frosted.dart';
import '../widgets/spectra_charts.dart';

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
  late final Set<String> _enabled;
  late final Map<String, Knobs> _knobs;
  double _temp = 2.0;
  String? _previewClass;

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
  }

  Color _classColor(String label) =>
      Cozy.accents[RulesModel.classLabels.indexOf(label) % Cozy.accents.length];

  // ── Derived ────────────────────────────────────────────────────────
  List<double> _stateOf(String label) => RulesModel.stateFromKnobs(_knobs[label]!);

  ({List<List<double>> w, List<double> b, int mask})? get _built {
    if (_enabled.isEmpty) return null;
    final states = {for (final l in _enabled) l: _stateOf(l)};
    return RulesModel.buildRulesModel(states, _enabled.toList(), temp: _temp);
  }

  bool get _selftestOk {
    final b = _built;
    if (b == null) return false;
    final states = {for (final l in _enabled) l: _stateOf(l)};
    return RulesModel.selftest(states, b.w, b.b, b.mask);
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
              label: 'boundary sharpness (temp)',
              hint: 'lower = crisper class boundaries',
              value: _temp,
              min: 1.0,
              max: 4.0,
              divisions: 30,
              format: (v) => v.toStringAsFixed(1),
              color: Cozy.linenAlmond,
              onChanged: (v) => setState(() => _temp = v),
            ),

            for (final label in RulesModel.classLabels)
              _ClassCard(
                label: label,
                color: _classColor(label),
                knobs: _knobs[label]!,
                enabled: _enabled.contains(label),
                onChanged: (on) => setState(
                    () => on ? _enabled.add(label) : _enabled.remove(label)),
                onKnob: (k, v) => setState(() => _knobs[label]![k] = v),
                onFocus: _focusPreview,
                onBlur: _blurPreview,
              ),

            const SizedBox(height: 16),
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
      final st = _stateOf(shown);
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
  final ValueChanged<bool> onChanged;
  final void Function(String key, double v) onKnob;
  final ValueChanged<String> onFocus;
  final VoidCallback onBlur;

  const _ClassCard({
    required this.label,
    required this.color,
    required this.knobs,
    required this.enabled,
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
          AnimatedSwitcher(
            duration: const Duration(milliseconds: 250),
            child: !enabled
                ? const SizedBox(width: double.infinity)
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
