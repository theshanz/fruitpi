import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/cozy_palette.dart';
import '../core/rules_model32d.dart';
import '../core/training_repository.dart';
import '../services/ble_service.dart';
import '../widgets/feature_views.dart';
import '../widgets/frosted.dart';
import '../widgets/spectra_charts.dart';
import '../widgets/waveform_chart.dart';
import 'data_collection_screen.dart';
import 'datasets_screen.dart';

/// Master Rule Builder & Category Engine dashboard — the 32-D Fruit Profile
/// pipeline.
///
/// Screen 1: global sensing balance (3 block weights), the up-to-5 category
/// cards, a live separation/health matrix, and Train & Upload. Each category
/// opens Screen 2 (the fine-tuning sheet) for its data source + 6 physical
/// knobs.
class RuleBuilderScreen extends StatefulWidget {
  final BleService bleService;
  const RuleBuilderScreen({super.key, required this.bleService});

  @override
  State<RuleBuilderScreen> createState() => _RuleBuilderScreenState();
}

class _RuleBuilderScreenState extends State<RuleBuilderScreen> {
  final _repo = TrainingRepository.instance;
  late FruitProfile _profile;
  late final TextEditingController _nameController;

  @override
  void initState() {
    super.initState();
    _profile = FruitProfile(name: 'Mango');
    _nameController = TextEditingController(text: _profile.name);
    // Defaults per spec: first 2 classes active, sensible archetype presets.
    const presets = [
      'hard_unripe',
      'prime_ripe',
      'overripe_soft',
      'hollow_defect',
      'inert_standard',
    ];
    for (var i = 0; i < _profile.categories.length; i++) {
      final c = _profile.categories[i];
      c.sourceType = CategorySourceType.archetype;
      c.archetypePresetId = presets[i];
      c.isEnabled = i < 2;
    }
    _repo.addListener(_onRepoChanged);
  }

  @override
  void dispose() {
    _nameController.dispose();
    _repo.removeListener(_onRepoChanged);
    super.dispose();
  }

  void _onRepoChanged() {
    if (mounted) setState(() {});
  }

  Color _slotColor(int c) => Cozy.accents[c % Cozy.accents.length];

  // ── Derived preview ────────────────────────────────────────────────
  Map<String, List<double>> get _measuredCentroids =>
      _repo.centroids32dFor(_profile.activeLabels);

  Map<String, List<List<double>>> get _measuredStates =>
      _repo.states32dFor(_profile.activeLabels);

  List<List<double>> get _compiledProtos => FruitProfileCompiler
      .compiledPrototypes(_profile,
          measuredCentroids: _measuredCentroids,
          measuredStates: _measuredStates);

  List<double> get _compiledInvVar => FruitProfileCompiler
      .compiledInverseVariance(_profile,
          measuredCentroids: _measuredCentroids,
          measuredStates: _measuredStates);

  List<List<double>> get _sepMatrix =>
      FruitProfileCompiler.mahalanobisMatrix(
        _compiledProtos,
        _compiledInvVar,
      );

  /// Minimum pairwise Mahalanobis d' among active prototype pairs = model
  /// health margin. Higher d' = better separated (1.0 = one sigma).
  double? get _minMargin {
    final active = _profile.activeLabels;
    if (active.length < 2) return null;
    var minD = double.infinity;
    for (var i = 0; i < active.length; i++) {
      for (var j = i + 1; j < active.length; j++) {
        final a = RulesModel32D.indexOf(active[i]);
        final b = RulesModel32D.indexOf(active[j]);
        final d = _sepMatrix[a][b];
        if (d < minD) minD = d;
      }
    }
    return minD;
  }

  bool get _selftestOk {
    final active = _profile.activeLabels;
    if (active.length < 2) return false;
    final m = _minMargin;
    return m != null && m >= 1.0;
  }

  String get _healthLabel {
    final m = _minMargin;
    if (m == null) return '—';
    if (m >= 3.5) return 'EXCELLENT';
    if (m >= 2.2) return 'GOOD';
    if (m >= 1.0) return 'DISTINCT';
    return 'WEAK';
  }

  Color _healthColor() {
    final m = _minMargin;
    if (m == null) return Cozy.dimGray;
    if (m >= 3.5) return Cozy.mintCyan;
    if (m >= 2.2) return Cozy.matcha;
    if (m >= 1.0) return Cozy.chamomile;
    return Cozy.roseError;
  }

  // ── Actions ────────────────────────────────────────────────────────
  Future<void> _openCategory(int id) async {
    final edited = await Navigator.push<CategoryRule>(
      context,
      MaterialPageRoute(
        builder: (_) => _CategorySheet(
          bleService: widget.bleService,
          profile: _profile,
          rule: _profile.categories[id],
          measuredCentroids: _measuredCentroids,
          onDirty: _onRepoChanged,
        ),
      ),
    );
    if (edited != null && mounted) {
      setState(() => _profile.categories[id] = edited);
    }
  }

  Future<void> _reset() async {
    setState(() {
      _profile.name = 'Mango';
      _nameController.text = _profile.name;
      _profile.wOptics = 1.0;
      _profile.wImage = 1.0;
      _profile.wMoments = 1.0;
      for (var i = 0; i < _profile.categories.length; i++) {
        final c = _profile.categories[i];
        c.isEnabled = i < 2;
        c.sourceType = CategorySourceType.archetype;
        c.archetypePresetId = const [
          'hard_unripe', 'prime_ripe', 'overripe_soft', 'hollow_defect',
          'inert_standard',
        ][i];
        // UNRIPE (slot 0) is the green skin — a green veto would reject it.
        c.enableGreenVeto = i != 0;
        c.greenVetoThreshold = 0.35;
        c.knobs = CategoryKnobs();
      }
    });
  }

  Future<void> _trainAndUpload() async {
    final name = _profile.name.trim();
    if (name.isEmpty) {
      _toast('Give the fruit a name');
      return;
    }
    if (_profile.activeCount < 2) {
      _toast('Enable at least 2 categories to train');
      return;
    }
    if (!_selftestOk) {
      _toast('Prototypes are too close — adjust knobs to separate classes');
      return;
    }
    final bin = FruitProfileCompiler.compile(_profile,
        measuredCentroids: _measuredCentroids,
        measuredStates: _measuredStates);

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
                  fontFamily: Cozy.monoFamily,
                  fontSize: 14,
                  color: Cozy.matcha)),
          content: Column(mainAxisSize: MainAxisSize.min, children: [
            ValueListenableBuilder<double>(
              valueListenable: notifier,
              builder: (_, v, __) => LinearProgressIndicator(
                  value: v > 0 ? v : null, minHeight: 8),
            ),
            const SizedBox(height: 12),
            Text('streaming to ESP32 flash…',
                style: TextStyle(fontSize: 15, color: Cozy.warmGray)),
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
    await Clipboard.setData(
        ClipboardData(text: RulesModel32D.toBase64(bin)));
    _toast('No device connected — base64 .bin copied to clipboard');
  }

  Future<void> _loadFromDisk() async {
    if (Platform.isLinux && _repo.pickedBaseDir == null) {
      final ok = await _repo.pickBaseDir();
      if (!ok) return;
    }
    final n = await _repo.loadFromDisk();
    _toast(n == 0
        ? 'No new saved sessions found on disk'
        : 'Loaded $n saved session(s)');
  }

  Future<void> _openDatasets() async {
    await Navigator.push<void>(
      context,
      MaterialPageRoute(
        builder: (_) => DatasetsScreen(
          repo: _repo,
          categoryOptions: [
            for (final c in _profile.categories) c.name,
          ],
        ),
      ),
    );
    if (mounted) setState(() {});
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
    final active = _profile.activeLabels;

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
            tooltip: 'Manage datasets',
            icon: const Icon(Icons.dashboard_customize_rounded),
            onPressed: _openDatasets,
          ),
          IconButton(
            tooltip: 'Reset profile & knobs',
            icon: const Icon(Icons.restart_alt_rounded),
            onPressed: _reset,
          ),
          IconButton(
            tooltip: 'Copy .bin as base64',
            icon: const Icon(Icons.copy_all_rounded),
            onPressed: () {
              final bin = FruitProfileCompiler.compile(_profile,
                  measuredCentroids: _measuredCentroids,
                  measuredStates: _measuredStates);
              _exportToClipboard(bin);
            },
          ),
        ],
      ),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            _nameField(),
            const SizedBox(height: 12),
            _healthBanner(ok),
            const SizedBox(height: 16),
            _balanceCard(),
            const SizedBox(height: 18),
            _categoriesSection(),
            const SizedBox(height: 18),
            _separationCard(active),
            const SizedBox(height: 20),
            FilledButton.icon(
              style: FilledButton.styleFrom(
                minimumSize: const Size.fromHeight(56),
                backgroundColor: Cozy.matcha,
                foregroundColor: Cozy.deepBg,
                shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(18)),
              ),
              onPressed:
                  _profile.activeCount < 2 || !ok ? null : _trainAndUpload,
              icon: const Icon(Icons.rocket_launch_rounded),
              label: Text(
                widget.bleService.isConnected
                    ? 'TRAIN & UPLOAD'
                    : 'TRAIN & EXPORT',
                style: const TextStyle(
                    fontWeight: FontWeight.bold, letterSpacing: 1.2),
              ),
            ),
            const SizedBox(height: 24),
          ],
        ),
      ),
    );
  }

  Widget _nameField() {
    return TextField(
      controller: _nameController,
      maxLength: 31,
      style: const TextStyle(fontFamily: Cozy.monoFamily, fontSize: 16),
      decoration: InputDecoration(
        labelText: 'FRUIT PROFILE NAME',
        labelStyle: TextStyle(fontSize: 15, color: Cozy.dimGray),
        prefixIcon: const Icon(Icons.local_florist_rounded, color: Cozy.matcha),
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
      onChanged: (v) => setState(() => _profile.name = v),
    );
  }

  Widget _healthBanner(bool ok) {
    final active = _profile.activeCount;
    final health = _healthLabel;
    return FrostedBox(
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
            size: 17, color: ok ? Cozy.matcha : Cozy.roseError),
        const SizedBox(width: 10),
        Expanded(
          child: Text(
            ok
                ? 'SELFTEST OK · HEALTH $health'
                : active < 2
                    ? 'ENABLE AT LEAST 2 CATEGORIES'
                    : 'SELFTEST FAILED — SEPARATE THE CLASSES',
            style: TextStyle(
                fontSize: 14.5,
                letterSpacing: 0.6,
                fontWeight: FontWeight.bold,
                color: ok ? Cozy.matcha : Cozy.roseError),
          ),
        ),
      ]),
    );
  }

  Widget _balanceCard() {
    return FrostedBox(
      padding: const EdgeInsets.all(16),
      borderRadius: BorderRadius.circular(20),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        const SectionLabel(title: '// GLOBAL SENSING BALANCE'),
        const SizedBox(height: 4),
        Text('how much each physical domain drives the decision',
            style: TextStyle(fontSize: 14, color: Cozy.dimGray)),
        const SizedBox(height: 8),
        _BalanceSlider(
          icon: Icons.visibility_rounded,
          label: 'Skin Optics',
          hint: 'hue & dispersion',
          color: Cozy.chamomile,
          value: _profile.wOptics,
          onChanged: (v) => setState(() => _profile.wOptics = v),
        ),
        _BalanceSlider(
          icon: Icons.graphic_eq_rounded,
          label: 'Acoustic Texture',
          hint: '4x4 spectrogram',
          color: Cozy.duskBlue,
          value: _profile.wImage,
          onChanged: (v) => setState(() => _profile.wImage = v),
        ),
        _BalanceSlider(
          icon: Icons.monitor_heart_rounded,
          label: 'Flesh Dynamics',
          hint: 'damping, stiffness, tail',
          color: Cozy.heatherPink,
          value: _profile.wMoments,
          onChanged: (v) => setState(() => _profile.wMoments = v),
        ),
      ]),
    );
  }

  Widget _categoriesSection() {
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Row(children: [
        const Expanded(
            child: SectionLabel(title: '// CATEGORIES')),
        Text('${_profile.activeCount}/5 ACTIVE',
            style: TextStyle(
                fontSize: 14, fontWeight: FontWeight.bold, color: Cozy.matcha)),
      ]),
      const SizedBox(height: 8),
      for (var i = 0; i < _profile.categories.length; i++)
        _categoryCard(i, _profile.categories[i]),
    ]);
  }

  Widget _categoryCard(int id, CategoryRule rule) {
    final color = _slotColor(id);
    final measured = rule.sourceType == CategorySourceType.measured;
    final hasData = _repo.hasData(rule.name);
    final presets = measured && !hasData;
    return Card(
      margin: const EdgeInsets.only(bottom: 8),
      elevation: 0,
      color: rule.isEnabled
          ? const Color(0xFF1B1D22)
          : const Color(0xFF141519),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(16),
        side: BorderSide(
            color: rule.isEnabled
                ? color.withValues(alpha: 0.4)
                : Colors.white.withValues(alpha: 0.05),
            width: 1.2),
      ),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(12, 4, 8, 8),
        child: Column(children: [
          Row(children: [
            Container(
              width: 12, height: 12,
              decoration:
                  BoxDecoration(color: color, shape: BoxShape.circle),
            ),
            const SizedBox(width: 8),
            Icon(measured ? Icons.storage_rounded : Icons.auto_awesome,
                size: 15, color: color),
            const SizedBox(width: 8),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(rule.name.replaceAll('_', ' '),
                      style: TextStyle(
                          fontFamily: Cozy.monoFamily,
                          fontSize: 16,
                          fontWeight: FontWeight.w800,
                          letterSpacing: 0.8,
                          color:
                              rule.isEnabled ? Cozy.oatmeal : Cozy.dimGray)),
                  Text(
                    _sourceLine(rule, measured, hasData),
                    style:
                        TextStyle(fontSize: 13, color: Cozy.dimGray),
                    overflow: TextOverflow.ellipsis,
                  ),
                ],
              ),
            ),
            if (presets)
              Icon(Icons.warning_amber_rounded,
                  size: 15, color: Cozy.chamomile),
            Switch(
              value: rule.isEnabled,
              activeTrackColor: color.withValues(alpha: 0.6),
              onChanged: (v) =>
                  setState(() => rule.isEnabled = v),
            ),
            TextButton.icon(
              style: TextButton.styleFrom(foregroundColor: color),
              onPressed: () => _openCategory(id),
              icon: const Icon(Icons.tune_rounded, size: 16),
              label: const Text('EDIT',
                  style: TextStyle(fontWeight: FontWeight.bold, fontSize: 14)),
            ),
          ]),
          if (rule.isEnabled) ...[
            const Divider(height: 1, color: Colors.white12),
            const SizedBox(height: 4),
            Row(children: [
              const SizedBox(width: 28),
              Expanded(
                child: Text(
                  _knobLine(rule),
                  style: TextStyle(fontSize: 13, color: Cozy.warmGray),
                  overflow: TextOverflow.ellipsis,
                ),
              ),
            ]),
          ],
        ]),
      ),
    );
  }

  String _sourceLine(CategoryRule rule, bool measured, bool hasData) {
    if (measured) {
      return hasData
          ? '${_repo.sessionsFor(rule.name).fold<int>(0, (a, s) => a + s.tapCount)} taps recorded'
          : 'measured — record taps first';
    }
    final a = ArchetypeLibrary.byId(rule.archetypePresetId);
    return 'archetype · ${a?.displayName ?? rule.archetypePresetId ?? ''}';
  }

  String _knobLine(CategoryRule rule) {
    final k = rule.knobs;
    final b = <String>[];
    if (k.stiffnessDelta.abs() > 0.01) b.add('stiff ${_s(k.stiffnessDelta)}');
    if (k.dampingDelta.abs() > 0.01) b.add('damp ${_s(k.dampingDelta)}');
    if (k.resonanceTailDelta.abs() > 0.01) b.add('ring ${_s(k.resonanceTailDelta)}');
    if (k.highToneHashDelta.abs() > 0.01) b.add('hash ${_s(k.highToneHashDelta)}');
    if (k.hueDeltaDeg.abs() > 0.5) b.add('hue ${_s(k.hueDeltaDeg)}');
    if (b.isEmpty) return 'baseline prototype · no knob offsets';
    return 'knobs: ${b.join(' · ')}';
  }

  static String _s(double v) => v >= 0 ? '+${v.toStringAsFixed(1)}' : v.toStringAsFixed(1);

  Widget _separationCard(List<String> active) {
    return FrostedBox(
      padding: const EdgeInsets.all(16),
      borderRadius: BorderRadius.circular(20),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        const SectionLabel(title: '// MODEL HEALTH & SEPARATION'),
        const SizedBox(height: 4),
        Text('live separation of active class means',
            style: TextStyle(fontSize: 14, color: Cozy.dimGray)),
        const SizedBox(height: 10),
        if (active.length < 2)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 12),
            child: Center(
              child: Text('enable ≥ 2 categories to measure separation',
                  style: TextStyle(fontSize: 14.5, color: Cozy.dimGray)),
            ),
          )
        else ...[
          for (var i = 0; i < active.length; i++)
            for (var j = i + 1; j < active.length; j++)
              _sepRow(active[i], active[j]),
          const SizedBox(height: 8),
          Row(children: [
            Icon(Icons.monitor_heart_rounded,
                size: 16, color: _healthColor()),
            const SizedBox(width: 8),
            Expanded(
              child: Text(
                'Overall Health: ${_healthLabel}'
                '${_minMargin != null ? ' (min separation ${_minMargin!.toStringAsFixed(2)})' : ''}',
                style: TextStyle(
                    fontSize: 15,
                    fontWeight: FontWeight.bold,
                    color: _healthColor()),
              ),
            ),
          ]),
        ],
      ]),
    );
  }

  Widget _sepRow(String a, String b) {
    final ia = RulesModel32D.indexOf(a);
    final ib = RulesModel32D.indexOf(b);
    final dprime = _sepMatrix[ia][ib];
    // d' can exceed the p10-scale of a 0-1 progress bar; clamp to a display
    // ceiling of 4.0 (>= EXCELLENT) so the bar fills sensibly while the label
    // still prints the true value.
    final progress = (dprime / 4.0).clamp(0.0, 1.0);
    final color = dprime >= 3.5
        ? Cozy.mintCyan
        : dprime >= 2.2
            ? Cozy.matcha
            : dprime >= 1.0
                ? Cozy.chamomile
                : Cozy.roseError;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 3),
      child: Row(children: [
        Expanded(
          flex: 2,
          child: Text(_abbrev(a),
              overflow: TextOverflow.ellipsis,
              style: TextStyle(
                  fontSize: 13,
                  fontWeight: FontWeight.bold,
                  color: _slotColor(ia))),
        ),
        SizedBox(width: 8),
        Expanded(
          flex: 3,
          child: Column(crossAxisAlignment: CrossAxisAlignment.end, children: [
            ClipRRect(
              borderRadius: BorderRadius.circular(4),
              child: LinearProgressIndicator(
                value: progress,
                minHeight: 6,
                backgroundColor: Colors.white.withValues(alpha: 0.08),
                valueColor: AlwaysStoppedAnimation(
                    color.withValues(alpha: 0.8)),
              ),
            ),
            const SizedBox(height: 2),
            Text("d' ${dprime.toStringAsFixed(2)}",
                style: TextStyle(
                    fontSize: 12, fontWeight: FontWeight.bold, color: color)),
          ]),
        ),
        SizedBox(width: 8),
        Expanded(
          flex: 2,
          child: Text(_abbrev(b),
              textAlign: TextAlign.right,
              overflow: TextOverflow.ellipsis,
              style: TextStyle(
                  fontSize: 13,
                  fontWeight: FontWeight.bold,
                  color: _slotColor(ib))),
        ),
      ]),
    );
  }

  /// Clean short label for a category slot.
  static String _abbrev(String s) {
    if (s.toUpperCase() == 'UNRIPE') return 'UNRIPE';
    if (s.toUpperCase() == 'PERFECTLY RIPE') return 'PERFECTLY RIPE';
    return s.replaceAll('_', ' ');
  }
}

class _BalanceSlider extends StatelessWidget {
  final IconData icon;
  final String label, hint;
  final Color color;
  final double value;
  final ValueChanged<double> onChanged;
  const _BalanceSlider(
      {required this.icon,
      required this.label,
      required this.hint,
      required this.color,
      required this.value,
      required this.onChanged});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Row(children: [
        Icon(icon, size: 18, color: color),
        const SizedBox(width: 10),
        Expanded(
          flex: 3,
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(label,
                style: const TextStyle(
                    fontSize: 12.5, fontWeight: FontWeight.w700, color: Cozy.oatmeal)),
            Text(hint, style: TextStyle(fontSize: 14, color: Cozy.dimGray)),
          ]),
        ),
        Expanded(
          flex: 5,
          child: SliderTheme(
            data: SliderTheme.of(context).copyWith(
              trackHeight: 5,
              activeTrackColor: color.withValues(alpha: 0.85),
              inactiveTrackColor: Colors.white.withValues(alpha: 0.08),
              thumbColor: color,
              overlayColor: color.withValues(alpha: 0.15),
            ),
            child: Slider(
              value: value,
              min: 0.1,
              max: 2.0,
              divisions: 38,
              onChanged: onChanged,
            ),
          ),
        ),
        SizedBox(
          width: 42,
          child: Text('${(value * 100).round()}%',
              textAlign: TextAlign.right,
              style: TextStyle(
                  fontSize: 14, fontWeight: FontWeight.bold, color: color)),
        ),
      ]),
    );
  }
}

// ══════════════════════════════════════════════════════════════════════
// Screen 2: Category Rule & Fine-Tuning Sheet
// ══════════════════════════════════════════════════════════════════════
class _CategorySheet extends StatefulWidget {
  final BleService bleService;
  final FruitProfile profile;
  final CategoryRule rule;
  final Map<String, List<double>> measuredCentroids;
  final VoidCallback onDirty;
  const _CategorySheet(
      {required this.bleService,
      required this.profile,
      required this.rule,
      required this.measuredCentroids,
      required this.onDirty});

  @override
  State<_CategorySheet> createState() => _CategorySheetState();
}

class _CategorySheetState extends State<_CategorySheet> {
  late CategoryRule _draft;
  final _repo = TrainingRepository.instance;
  int? _tapCount;

  @override
  void initState() {
    super.initState();
    _draft = widget.rule.copy();
    _tapCount = _tapSessions();
  }

  int _tapSessions() =>
      _repo.sessionsFor(_draft.name).fold<int>(0, (a, s) => a + s.tapCount);

  Color get _color =>
      Cozy.accents[_draft.id % Cozy.accents.length];

  double get _naturalHue =>
      RulesModel32D.naturalHueFor(
          _draft.sourceType,
          _draft.archetypePresetId,
          _measuredCentroid);

  /// Green veto is meaningless on the green-skin UNRIPE class: a green mango
  /// should be accepted, not rejected.
  bool get _isUnripe => _draft.name.contains('UNRIPE');

  /// Live measured centroid queried directly from [TrainingRepository] — not a
  /// stale snapshot. Updates the moment a tap is saved, so the sliders, track
  /// center labels and the live preview reflect the recorded fruit immediately.
  List<double>? get _measuredCentroid =>
      _repo.centroid32dOf(_draft.name);

  double get _baselineStiffness =>
      _measuredCentroid != null ? _measuredCentroid![29] : 0.50;

  double get _baselineDamping =>
      _measuredCentroid != null ? _measuredCentroid![31] : 0.50;

  double get _baselineTail =>
      _measuredCentroid != null ? _measuredCentroid![27] : 0.20;

  double get _baselineHash =>
      _measuredCentroid != null ? _measuredCentroid![28] : 0.30;

  double get _hueAbs =>
      (_naturalHue + _draft.knobs.hueDeltaDeg).clamp(20.0, 120.0);

  void _setHueAbs(double target) {
    // Let the hue reach the full 20..120° window regardless of the source's
    // intrinsic colour — do NOT cap the offset at ±60°.
    final n = _naturalHue;
    final delta = (target - n).clamp(20.0 - n, 120.0 - n);
    setState(() => _draft.knobs.hueDeltaDeg = delta);
  }

  /// Compute this category's compiled (unit, weighted) prototype for preview.
  List<double> get _previewProto {
    // build a temp profile with only this slot enabled
    final p = FruitProfile(
        name: widget.profile.name,
        wOptics: widget.profile.wOptics,
        wImage: widget.profile.wImage,
        wMoments: widget.profile.wMoments);
    p.categories[_draft.id] = _draft;

    // Live states/centroids from the repository so the preview updates
    // immediately after recording instead of showing the stale snapshot.
    final liveCentroids = _repo.centroids32dFor([_draft.name]);
    final liveStates = _repo.states32dFor([_draft.name]);
    final protos = FruitProfileCompiler.compiledPrototypes(p,
        measuredCentroids: liveCentroids, measuredStates: liveStates);
    return protos[_draft.id];
  }

  Future<void> _recordTaps() async {
    final fruit = widget.profile.name.trim().isEmpty
        ? 'Mango'
        : widget.profile.name.trim();
    final saved = await Navigator.push<bool>(
      context,
      MaterialPageRoute(
        builder: (_) => DataCollectionScreen(
          bleService: widget.bleService,
          category: _draft.name,
          fruitName: fruit,
        ),
      ),
    );
    if (saved ?? false) {
      setState(() {
        _tapCount = _tapSessions();
        _draft.sourceType = CategorySourceType.measured;
      });
      widget.onDirty();
    }
  }

  Future<void> _editSession({String? id}) async {
    final sessions = _managedSessions;
    SampleSession? s;
    if (id != null) {
      for (final e in sessions) {
        if (e.id == id) {
          s = e;
          break;
        }
      }
    }
    s ??= sessions.isEmpty ? null : sessions.first;
    if (s == null) return;
    final session = s;
    final saved = await Navigator.push<bool>(
      context,
      MaterialPageRoute(
        builder: (_) => DataCollectionScreen(
          bleService: widget.bleService,
          category: _draft.name,
          fruitName: session.fruit,
          editSession: session,
        ),
      ),
    );
    if (saved ?? false) {
      setState(() => _tapCount = _tapSessions());
      widget.onDirty();
    }
  }

  List<SampleSession> get _managedSessions => _repo.sessionsFor(_draft.name);

  /// Sessions that could be assigned to this category (not already assigned).
  Iterable<SampleSession> get _assignableSessions => _repo.allSessions
      .where((s) => _repo.assignedCategory(s.id) != _draft.name);

  Future<void> _addDataset() async {
    final assignable = _assignableSessions.toList();
    if (assignable.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
          content: Text('No other saved datasets to add')));
      return;
    }
    final pick = await showModalBottomSheet<SampleSession>(
      context: context,
      backgroundColor: Cozy.surfaceCard,
      shape: const RoundedRectangleBorder(
          borderRadius: BorderRadius.vertical(top: Radius.circular(22))),
      builder: (sheet) => SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            const SectionLabel(title: '// ADD SAVED DATASET TO THIS CATEGORY'),
            const SizedBox(height: 6),
            for (final s in assignable)
              ListTile(
                contentPadding: EdgeInsets.zero,
                leading: Icon(Icons.storage_rounded,
                    size: 18, color: Cozy.duskBlue),
                title: Text(s.fruit,
                    style: const TextStyle(
                        fontSize: 16, color: Cozy.oatmeal)),
                subtitle: Text(
                    '${s.tapCount} taps · ${s.hueCount} hues · currently “${_repo.assignedCategory(s.id)}”',
                    style: const TextStyle(fontSize: 12.5, color: Cozy.dimGray)),
                onTap: () => Navigator.pop(sheet, s),
              ),
          ],
        ),
      ),
    );
    if (pick != null && mounted) {
      await _repo.assignSession(pick.id, _draft.name);
      setState(() => _tapCount = _tapSessions());
      widget.onDirty();
    }
  }

  Future<void> _inspectDataset(SampleSession s) async {
    final cat = _repo.assignedCategory(s.id);
    final idx = _repo.allSessions.indexWhere((x) => x.id == s.id);
    final color = Cozy.accents[idx < 0 ? 0 : (idx % Cozy.accents.length)];
    showModalBottomSheet<void>(
      context: context,
      backgroundColor: Cozy.surfaceCard,
      isScrollControlled: true,
      shape: const RoundedRectangleBorder(
          borderRadius: BorderRadius.vertical(top: Radius.circular(22))),
      builder: (sheet) => DraggableScrollableSheet(
        initialChildSize: 0.8,
        maxChildSize: 0.98,
        minChildSize: 0.5,
        expand: false,
        builder: (_, scrollCtrl) => ListView(
          controller: scrollCtrl,
          padding: const EdgeInsets.fromLTRB(18, 20, 18, 30),
          children: [
            Row(children: [
              Text('DATASET · ${s.fruit}',
                  style: const TextStyle(
                      fontFamily: Cozy.monoFamily,
                      fontSize: 15,
                      fontWeight: FontWeight.w800,
                      color: Cozy.oatmeal)),
              const Spacer(),
              _categoryChip(cat, color),
            ]),
            const SizedBox(height: 4),
            Text(
                '${s.tapCount} taps · ${s.hueCount} hues · '
                '${s.aggregateHue?.volumeCm3.toStringAsFixed(0) ?? '—'} cm³',
                style: TextStyle(fontSize: 13, color: Cozy.dimGray)),
            const SizedBox(height: 14),
            const SectionLabel(title: '// TAPS (per-tap waveform + spectrum)'),
            const SizedBox(height: 6),
            for (var i = 0; i < s.taps.length; i++)
              Padding(
                padding: const EdgeInsets.only(bottom: 8),
                child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text('TAP ${i + 1}',
                          style: const TextStyle(
                              fontFamily: Cozy.monoFamily,
                              fontSize: 12,
                              fontWeight: FontWeight.w800,
                              color: Cozy.oatmeal)),
                      const SizedBox(height: 4),
                      SizedBox(
                          height: 100,
                          child: WaveformChart(samples: s.taps[i].waveform)),
                      const SizedBox(height: 4),
                      SizedBox(
                          height: 100,
                          child: SpectrumChart(
                              series: PlotSeries('t${i + 1}', color,
                                  s.taps[i].spectrum))),
                    ]),
              ),
            const SizedBox(height: 12),
            OutlinedButton.icon(
              style: OutlinedButton.styleFrom(
                foregroundColor: Cozy.dimGray,
                side: BorderSide(color: Colors.white.withValues(alpha: 0.1)),
              ),
              onPressed: () => Navigator.pop(sheet),
              icon: const Icon(Icons.close_rounded, size: 18),
              label: const Text('CLOSE',
                  style: TextStyle(fontWeight: FontWeight.bold)),
            ),
          ],
        ),
      ),
    );
  }

  Future<void> _unassignDataset(SampleSession s) async {
    await _repo.assignSession(s.id, '');
    setState(() => _tapCount = _tapSessions());
    widget.onDirty();
  }

  Widget _categoryChip(String label, Color color) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
        decoration: BoxDecoration(
          color: color.withValues(alpha: 0.14),
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: color.withValues(alpha: 0.5)),
        ),
        child: Text(label.replaceAll('_', ' '),
            style: TextStyle(
                fontSize: 12, fontWeight: FontWeight.bold, color: color)),
      );

  void _resetBaseline() {
    setState(() {
      _draft.sourceType = CategorySourceType.archetype;
      _draft.knobs = CategoryKnobs();
      _draft.enableGreenVeto = !_draft.name.contains('UNRIPE');
      _draft.greenVetoThreshold = 0.35;
      _tapCount = _tapSessions();
    });
  }

  @override
  Widget build(BuildContext context) {
    final proto = _previewProto;
    return Scaffold(
      appBar: AppBar(
        title: Text('Category · ${_draft.name.replaceAll('_', ' ')}'),
        actions: [
          Padding(
            padding: const EdgeInsets.only(right: 12),
            child: TextButton(
              style: TextButton.styleFrom(
                padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
              ),
              onPressed: () => Navigator.pop(context, _draft),
              child: const Text('DONE',
                  style: TextStyle(
                      fontWeight: FontWeight.bold, color: Cozy.matcha)),
            ),
          ),
        ],
      ),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            // ── Source ──
            FrostedBox(
              padding: const EdgeInsets.all(16),
              borderRadius: BorderRadius.circular(20),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const SectionLabel(title: '// DATA SOURCE'),
                  const SizedBox(height: 10),
                  Row(children: [
                    _sourceOption(
                        label: 'Measured Taps',
                        helper: '$_tapCount taps',
                        icon: Icons.storage_rounded,
                        selected:
                            _draft.sourceType == CategorySourceType.measured,
                        onTap: () => setState(() =>
                            _draft.sourceType = CategorySourceType.measured)),
                    const SizedBox(width: 8),
                    _sourceOption(
                        label: 'Archetype Preset',
                        helper: 'golden baseline',
                        icon: Icons.auto_awesome_rounded,
                        selected:
                            _draft.sourceType == CategorySourceType.archetype,
                        onTap: () => setState(() =>
                            _draft.sourceType = CategorySourceType.archetype)),
                  ]),
                  const SizedBox(height: 10),
                  if (_draft.sourceType == CategorySourceType.measured)
                    Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                      Row(children: [
                        OutlinedButton.icon(
                          onPressed: _recordTaps,
                          icon: const Icon(Icons.add_link_rounded, size: 16),
                          label: const Text('RECORD MORE TAPS',
                              style: TextStyle(fontWeight: FontWeight.bold, fontSize: 14)),
                        ),
                        const SizedBox(width: 8),
                        TextButton.icon(
                          onPressed: _addDataset,
                          icon: const Icon(Icons.library_add_rounded, size: 15),
                          label: const Text('ADD FROM SAVED',
                              style: TextStyle(fontSize: 14)),
                        ),
                        if (_managedSessions.isNotEmpty) ...[
                          const SizedBox(width: 8),
                          TextButton.icon(
                            onPressed: () =>
                                _editSession(id: _managedSessions.first.id),
                            icon: const Icon(Icons.edit_outlined, size: 15),
                            label: const Text('EDIT',
                                style: TextStyle(fontSize: 14)),
                          ),
                        ],
                      ]),
                      if (_managedSessions.isNotEmpty) ...[
                        const SizedBox(height: 8),
                        const SectionLabel(
                            title: '// MANAGED SESSIONS (feeds this category)'),
                        const SizedBox(height: 4),
                        for (final s in _managedSessions)
                          Padding(
                            padding: const EdgeInsets.only(bottom: 4),
                            child: Row(children: [
                              const Icon(Icons.storage_rounded,
                                  size: 15, color: Cozy.duskBlue),
                              const SizedBox(width: 6),
                              Expanded(
                                child: Text('${s.fruit} · ${s.tapCount} taps',
                                    overflow: TextOverflow.ellipsis,
                                    style: const TextStyle(
                                        fontSize: 13, color: Cozy.warmGray)),
                              ),
                              TextButton(
                                style: TextButton.styleFrom(
                                    foregroundColor: Cozy.chamomile,
                                    padding: const EdgeInsets.symmetric(
                                        horizontal: 6),
                                    minimumSize: const Size(0, 32)),
                                onPressed: () => _inspectDataset(s),
                                child: const Text('INSPECT',
                                    style: TextStyle(fontSize: 12)),
                              ),
                              TextButton(
                                style: TextButton.styleFrom(
                                    foregroundColor: Cozy.dimGray,
                                    padding: const EdgeInsets.symmetric(
                                        horizontal: 6),
                                    minimumSize: const Size(0, 32)),
                                onPressed: () => _unassignDataset(s),
                                child: const Text('REMOVE',
                                    style: TextStyle(fontSize: 12)),
                              ),
                            ]),
                          ),
                      ],
                    ])
                  else
                    DropdownButtonFormField<String>(
                      initialValue: _draft.archetypePresetId,
                      dropdownColor: Cozy.surfaceCard,
                      style: const TextStyle(fontSize: 16, color: Cozy.oatmeal),
                      decoration: InputDecoration(
                        labelText: 'PRE-CALIBRATED PRESET',
                        labelStyle: TextStyle(fontSize: 15, color: Cozy.dimGray),
                        filled: true,
                        fillColor: Colors.white.withValues(alpha: 0.03),
                        border: OutlineInputBorder(
                            borderRadius: BorderRadius.circular(12),
                            borderSide:
                                BorderSide(color: Colors.white.withValues(alpha: 0.08))),
                      ),
                      items: [
                        for (final a in ArchetypeLibrary.all)
                          DropdownMenuItem(
                              value: a.id,
                              child: Text('${a.displayName} — ${a.description}',
                                  overflow: TextOverflow.ellipsis,
                                  style: const TextStyle(fontSize: 15))),
                      ],
                      onChanged: (v) => setState(() {
                        _draft.archetypePresetId = v;
                        _draft.knobs.hueDeltaDeg = 0.0;
                      }),
                    ),
                ],
              ),
            ),
            const SizedBox(height: 14),

            // ── Optics & Skin ──
            FrostedBox(
              padding: const EdgeInsets.all(16),
              borderRadius: BorderRadius.circular(20),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const SectionLabel(title: '1 · OPTICS & SKIN (DIMS 0..9)'),
                  const SizedBox(height: 8),
                  _knobSlider(
                    label: 'Skin Hue Center',
                    value: _hueAbs,
                    min: 20, max: 120,
                    format: (v) => '${v.toStringAsFixed(1)}°',
                    color: _color,
                    onChanged: _setHueAbs,
                  ),
                  _knobSlider(
                    label: 'Hue Uniformity',
                    hint: 'blotchiness of colouring',
                    value: _draft.knobs.hueSpread,
                    min: 0, max: 1,
                    format: (v) => v.toStringAsFixed(2),
                    color: _color.withValues(alpha: 0.75),
                    onChanged: (v) =>
                        setState(() => _draft.knobs.hueSpread = v),
                  ),
                  SwitchListTile.adaptive(
                    contentPadding: EdgeInsets.zero,
                    dense: true,
                    activeThumbColor: Cozy.chamomile,
                    value: _draft.enableGreenVeto,
                    onChanged: _isUnripe
                        ? null
                        : (v) =>
                            setState(() => _draft.enableGreenVeto = v),
                    secondary: Icon(
                        _isUnripe
                            ? Icons.block_rounded
                            : Icons.verified_user_rounded,
                        color: Cozy.chamomile, size: 18),
                    title: Text(
                        _isUnripe
                            ? 'Strict Green Veto (not applicable)'
                            : 'Strict Green Veto',
                        style: TextStyle(
                            fontSize: 15.5,
                            fontWeight: FontWeight.w700,
                            color: _isUnripe ? Cozy.dimGray : Cozy.oatmeal)),
                    subtitle: Text(
                        _isUnripe
                            ? 'unripe is the green class — accept it'
                            : 'reject this class if skin is too green',
                        style: const TextStyle(fontSize: 13)),
                  ),
                  if (_draft.enableGreenVeto && !_isUnripe)
                    _knobSlider(
                      label: 'Green Hue Limit',
                      value: _draft.greenVetoThreshold,
                      min: 0.1, max: 0.6,
                      format: (v) => '${(v * 100).round()}%',
                      color: Cozy.matcha,
                      onChanged: (v) =>
                          setState(() => _draft.greenVetoThreshold = v),
                    ),
                ],
              ),
            ),
            const SizedBox(height: 14),

            // ── Flesh Acoustics ──
            FrostedBox(
              padding: const EdgeInsets.all(16),
              borderRadius: BorderRadius.circular(20),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const SectionLabel(title: '2 · FLESH ACOUSTICS (DIMS 10..31)'),
                  const SizedBox(height: 8),
                  _anchorSlider(
                    name: 'Flesh Damping',
                    baseline: _baselineDamping,
                    delta: _draft.knobs.dampingDelta,
                    hint: 'quick thud · energy absorbs fast',
                    color: Cozy.heatherPink,
                    endLeft: '-1 ringy',
                    endRight: '+1 dead',
                    onDelta: (v) =>
                        setState(() => _draft.knobs.dampingDelta = v),
                  ),
                  _anchorSlider(
                    name: 'Stiffness',
                    baseline: _baselineStiffness,
                    delta: _draft.knobs.stiffnessDelta,
                    hint: 'soft flesh · resonance shifts down',
                    color: Cozy.linenAlmond,
                    endLeft: '-1 soft',
                    endRight: '+1 stiff',
                    onDelta: (v) =>
                        setState(() => _draft.knobs.stiffnessDelta = v),
                  ),
                  _anchorSlider(
                    name: 'Resonance Ring',
                    baseline: _baselineTail,
                    delta: _draft.knobs.resonanceTailDelta,
                    hint: 'tail energy · lingering ring',
                    color: Cozy.duskBlue,
                    endLeft: '-1 muted',
                    endRight: '+1 ringing',
                    onDelta: (v) =>
                        setState(() => _draft.knobs.resonanceTailDelta = v),
                  ),
                  _anchorSlider(
                    name: 'High-Tone Hash',
                    baseline: _baselineHash,
                    delta: _draft.knobs.highToneHashDelta,
                    hint: 'surface scratch vs clean thud',
                    color: Cozy.chamomile,
                    endLeft: '-1 clean',
                    endRight: '+1 hissy',
                    onDelta: (v) =>
                        setState(() => _draft.knobs.highToneHashDelta = v),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 14),

            // ── Live signature preview ──
            FrostedBox(
              padding: const EdgeInsets.all(16),
              borderRadius: BorderRadius.circular(20),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const SectionLabel(title: '// LIVE 32D SIGNATURE'),
                  const SizedBox(height: 10),
                  Text('SKIN OPTICS (8D)',
                      style: TextStyle(fontSize: 15, color: Cozy.dimGray)),
                  const SizedBox(height: 4),
                  SizedBox(
                      height: 110,
                      child: HueHistogramChart(
                          series: PlotSeries(_draft.name, _color, proto.sublist(0, 8)))),
                  const SizedBox(height: 14),
                  Text('ACOUSTIC TEXTURE · 4x4 SPECTROGRAM (16D)',
                      style: TextStyle(fontSize: 15, color: Cozy.dimGray)),
                  const SizedBox(height: 6),
                  ConstrainedBox(
                    constraints: const BoxConstraints(maxHeight: 130),
                    child: SpectrogramGrid(
                        values: proto.sublist(10, 26), color: _color),
                  ),
                  const SizedBox(height: 14),
                  Text('FLESH DYNAMICS · BIO-MOMENTS (6D)',
                      style: TextStyle(fontSize: 15, color: Cozy.dimGray)),
                  const SizedBox(height: 8),
                  FleshDynamicsBars(
                      values: proto.sublist(26, 32),
                      color: _color,
                      labels: const [
                        'centroid', 'tail', 'harmonic',
                        'stiffness', 'entropy', 'damping',
                      ]),
                ],
              ),
            ),
            const SizedBox(height: 14),
            OutlinedButton.icon(
              style: OutlinedButton.styleFrom(
                minimumSize: const Size.fromHeight(48),
                foregroundColor: Cozy.chamomile,
                side: BorderSide(color: Cozy.chamomile.withValues(alpha: 0.5)),
                shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(18)),
              ),
              onPressed: _resetBaseline,
              icon: const Icon(Icons.restart_alt_rounded, size: 18),
              label: const Text('RESET TO RAW BASELINE',
                  style: TextStyle(fontWeight: FontWeight.bold, letterSpacing: 1)),
            ),
            const SizedBox(height: 24),
          ],
        ),
      ),
    );
  }

  Widget _sourceOption(
      {required String label,
      required String helper,
      required IconData icon,
      required bool selected,
      required VoidCallback onTap}) {
    return Expanded(
      child: GestureDetector(
        onTap: onTap,
        child: Container(
          padding: const EdgeInsets.all(12),
          decoration: BoxDecoration(
            color: selected
                ? _color.withValues(alpha: 0.14)
                : Colors.white.withValues(alpha: 0.03),
            borderRadius: BorderRadius.circular(14),
            border: Border.all(
                color: selected
                    ? _color.withValues(alpha: 0.7)
                    : Colors.white.withValues(alpha: 0.08)),
          ),
          child: Column(children: [
            Icon(icon, size: 20, color: selected ? _color : Cozy.dimGray),
            const SizedBox(height: 6),
            Text(label,
                textAlign: TextAlign.center,
                style: TextStyle(
                    fontSize: 14.5,
                    fontWeight: FontWeight.bold,
                    color: selected ? Cozy.oatmeal : Cozy.warmGray)),
            Text(helper,
                style: TextStyle(fontSize: 12.5, color: Cozy.dimGray)),
          ]),
        ),
      ),
    );
  }

  /// Anchor-aware acoustic slider. When the category has measured data the
  /// thumb sits AT the measured baseline on a 0..1 range (so the real physical
  /// value is immediately visible) and dragging shifts it from there, mapping
  /// back to a [-1,1] delta for the model. In archetype mode it degrades to a
  /// plain [-1,1] offset slider.
  Widget _anchorSlider({
    required String name,
    required double baseline,
    required double delta,
    required String hint,
    required Color color,
    required String endLeft,
    required String endRight,
    required ValueChanged<double> onDelta,
  }) {
    final measured =
        _draft.sourceType == CategorySourceType.measured && _tapCount! > 0;
    final effective = (baseline + delta * 0.30).clamp(0.0, 1.0);
    final valueText = measured
        ? '${effective.toStringAsFixed(2)}'
            '${delta.abs() < 0.01 ? ' (Measured)' : ''}'
        : _signed(delta);
    return _knobSlider(
      label: '$name  ·  $valueText',
      hint: hint,
      value: measured ? effective : delta,
      min: measured ? 0 : -1,
      max: measured ? 1 : 1,
      format: (_) => '',
      color: color,
      endLeft: endLeft,
      endMid: measured
          ? '${baseline.toStringAsFixed(2)} measured'
          : '0 baseline',
      endRight: endRight,
      onChanged: measured
          ? (v) {
              var d = (v - baseline) / 0.30;
              if (d < -1) d = -1;
              if (d > 1) d = 1;
              onDelta(d);
            }
          : onDelta,
    );
  }

  Widget _knobSlider({
    required String label,
    required double value,
    required double min,
    required double max,
    required String Function(double) format,
    required Color color,
    required ValueChanged<double> onChanged,
    String? hint,
    String? endLeft,
    String? endMid,
    String? endRight,
  }) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(children: [
        Expanded(
          flex: 3,
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(format(value).isEmpty
                ? label
                : '$label  ·  ${format(value)}',
                style: const TextStyle(
                    fontSize: 12.5,
                    fontWeight: FontWeight.w700,
                    color: Cozy.oatmeal)),
            if (hint != null)
              Text(hint, style: TextStyle(fontSize: 14, color: Cozy.dimGray)),
          ]),
        ),
        Expanded(
          flex: 7,
          child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            SliderTheme(
              data: SliderTheme.of(context).copyWith(
                trackHeight: 5,
                activeTrackColor: color.withValues(alpha: 0.85),
                inactiveTrackColor: Colors.white.withValues(alpha: 0.08),
                thumbColor: color,
                overlayColor: color.withValues(alpha: 0.15),
              ),
              child: Slider(
                value: value.clamp(min, max),
                min: min,
                max: max,
                onChanged: onChanged,
              ),
            ),
            if (endLeft != null)
              Row(children: [
                Expanded(
                    child: Text(endLeft,
                        style: TextStyle(fontSize: 12, color: Cozy.dimGray))),
                Expanded(
                    child: Text(endMid ?? '',
                        textAlign: TextAlign.center,
                        style: TextStyle(
                            fontSize: 12,
                            color: Cozy.dimGray,
                            fontStyle: FontStyle.italic))),
                Expanded(
                    child: Text(endRight ?? '',
                        textAlign: TextAlign.right,
                        style: TextStyle(fontSize: 12, color: Cozy.dimGray))),
              ]),
          ]),
        ),
      ]),
    );
  }
}

String _signed(double v) => v > 0 ? '+${v.toStringAsFixed(2)}' : v.toStringAsFixed(2);

