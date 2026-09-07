import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/cozy_palette.dart';
import '../core/dual_model_wire.dart';
import '../core/model_vault.dart';
import '../core/protocol.dart';
import '../core/training_repository.dart';
import '../services/ble_service.dart';
import '../widgets/frosted.dart';
import 'data_collection_screen.dart';
import 'datasets_screen.dart';

/// Dual-Latent Rule Builder — category flow exactly like 32-D builder, but
/// uses dual-latent wire format (modality toggle + calibrated controls).
///
/// Brings back the dataset tooling of the legacy builder: LOAD FROM DISK,
/// the Datasets manager, per-class "add from saved / edit" session control,
/// plus the full calibration surface the Python training pipeline exposes
/// (r0_spec, alpha, DUAL weights, min_peak_adc, STFT feat-mode).
class DualRuleBuilderScreen extends StatefulWidget {
  final BleService bleService;
  const DualRuleBuilderScreen({super.key, required this.bleService});

  @override
  State<DualRuleBuilderScreen> createState() => _DualRuleBuilderScreenState();
}

class _DualRuleBuilderScreenState extends State<DualRuleBuilderScreen> {
  final _repo = TrainingRepository.instance;
  late int _mask;
  late int _modality;
  late final TextEditingController _nameController;
  late final TextEditingController _r0SpecCtrl;
  late final TextEditingController _alphaCtrl;
  late final TextEditingController _minPeakCtrl;

  late double _wAcoustic;
  late int _featMode;
  late final TextEditingController _stftPriorCtrl;

  @override
  void initState() {
    super.initState();
    _mask = 0x1F;
    _modality = 2; // DUAL
    _nameController = TextEditingController(text: 'Mango Dual');
    _r0SpecCtrl =
        TextEditingController(text: DualModelWire.canonR0Spec.toStringAsFixed(4));
    _alphaCtrl = TextEditingController(
        text: DualModelWire.canonAlpha.toStringAsFixed(1));
    _minPeakCtrl = TextEditingController(
        text: DualModelWire.canonMinPeakAdc.toString());
    _wAcoustic = DualModelWire.canonWAcoustic;
    _featMode = DualModelWire.canonFeatMode;
    _stftPriorCtrl = TextEditingController(
        text: DualModelWire.canonStftT1Prior.toStringAsFixed(2));
    _repo.addListener(_onRepo);
  }

  @override
  void dispose() {
    _repo.removeListener(_onRepo);
    _nameController.dispose();
    _r0SpecCtrl.dispose();
    _alphaCtrl.dispose();
    _minPeakCtrl.dispose();
    _stftPriorCtrl.dispose();
    super.dispose();
  }

  void _onRepo() {
    if (mounted) setState(() {});
  }

  Set<int> get _enabled => {
    for (var c = 0; c < BleProtocol.classLabels.length; c++)
      if (((_mask >> c) & 1) == 1) c
  };

  List<String> get _enabledLabels =>
      [for (final c in _enabled) BleProtocol.classLabels[c]];

  String get _modalityLabel =>
      _modality == 0 ? 'ACOUSTIC' : (_modality == 1 ? 'OPTICS' : 'DUAL');

  double get _wOptics => (1.0 - _wAcoustic);

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
          categoryOptions: BleProtocol.classLabels,
        ),
      ),
    );
  }

  Future<void> _upload() async {
    final wire = _repo.buildDualWire(
      fruitName: _nameController.text.trim(),
      enabledLabels: _enabledLabels,
      modalityLabel: _modalityLabel,
      activeMask: _mask,
      r0Spec: double.tryParse(_r0SpecCtrl.text.trim()) ??
          DualModelWire.canonR0Spec,
      r0Mom: DualModelWire.canonR0Mom,
      alpha:
          double.tryParse(_alphaCtrl.text.trim()) ?? DualModelWire.canonAlpha,
      beta: DualModelWire.canonBeta,
      tau: DualModelWire.canonTau,
      wAcoustic: _wAcoustic,
      wOptics: _wOptics,
      minPeakAdc: int.tryParse(_minPeakCtrl.text.trim()) ??
          DualModelWire.canonMinPeakAdc,
      featMode: _featMode,
      stftT1Prior:
          double.tryParse(_stftPriorCtrl.text.trim()) ??
              DualModelWire.canonStftT1Prior,
    );
    if (wire == null) {
      _toast('Enable ≥ 2 classes with saved data to train');
      return;
    }
    if (!widget.bleService.isConnected) {
      ModelVault.put(wire);
      await Clipboard.setData(
          ClipboardData(text: DualModelWire.binName(wire)));
      _toast('Not connected — wire name copied');
      return;
    }
    final ok = await widget.bleService.uploadModelBin(wire);
    if (ok) ModelVault.put(wire);
    _toast(ok ? 'DUAL MODEL UPLOADED' : 'UPLOAD FAILED');
  }

  void _toast(String msg) {
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        behavior: SnackBarBehavior.floating,
        duration: const Duration(seconds: 3),
        content: Text(msg)));
  }

  // ── Per-class session control (loaned from the legacy category sheet) ──

  Future<void> _record(String label) async {
    await Navigator.push<bool>(
      context,
      MaterialPageRoute(
        builder: (_) => DataCollectionScreen(
          bleService: widget.bleService,
          category: label,
          fruitName: _nameController.text.trim().isEmpty
              ? 'Fruit'
              : _nameController.text.trim(),
        ),
      ),
    );
  }

  Future<void> _addSaved(String label) async {
    final assignable = _repo.allSessions
        .where((s) => _repo.assignedCategory(s.id) != label)
        .toList();
    if (assignable.isEmpty) {
      _toast('No other saved datasets to add');
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
                leading: const Icon(Icons.storage_rounded,
                    size: 18, color: Cozy.duskBlue),
                title: Text(s.fruit,
                    style: const TextStyle(
                        fontSize: 16, color: Cozy.oatmeal)),
                subtitle: Text(
                    '${s.tapCount} taps · ${s.hueCount} hues · currently '
                    '“${_repo.assignedCategory(s.id)}”',
                    style: const TextStyle(fontSize: 12.5, color: Cozy.dimGray)),
                onTap: () => Navigator.pop(sheet, s),
              ),
          ],
        ),
      ),
    );
    if (pick != null && mounted) {
      await _repo.assignSession(pick.id, label);
    }
  }

  Future<void> _editSession(String label, SampleSession s) async {
    await Navigator.push<bool>(
      context,
      MaterialPageRoute(
        builder: (_) => DataCollectionScreen(
          bleService: widget.bleService,
          category: label,
          fruitName: s.fruit,
          editSession: s,
        ),
      ),
    );
  }

  Future<void> _removeSession(SampleSession s) async {
    await _repo.assignSession(s.id, '');
  }

  // ── Build ──────────────────────────────────────────────────────────

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Dual-Latent Builder'),
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
        ],
      ),
      body: SafeArea(
        child: ListView(padding: const EdgeInsets.all(16), children: [
          _modelCard(),
          const SizedBox(height: 16),
          for (final c in _enabled) ...[
            _classCard(c),
            const SizedBox(height: 10),
          ],
          const SizedBox(height: 6),
          _calibrationCard(),
          const SizedBox(height: 16),
          FilledButton.icon(
            style: FilledButton.styleFrom(
              minimumSize: const Size.fromHeight(54),
              backgroundColor: Cozy.matcha,
              foregroundColor: Cozy.deepBg,
              shape:
                  RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
            ),
            onPressed: _upload,
            icon: const Icon(Icons.save_rounded),
            label: Text(
              widget.bleService.isConnected
                  ? 'TRAIN & UPLOAD DUAL MODEL'
                  : 'TRAIN & EXPORT',
              style: const TextStyle(
                  fontWeight: FontWeight.bold, letterSpacing: 0.8),
            ),
          ),
          const SizedBox(height: 24),
        ]),
      ),
    );
  }

  Widget _modelCard() {
    return FrostedBox(
      borderRadius: BorderRadius.circular(22),
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const SectionLabel(title: '// DUAL-LATENT MODEL'),
            Text('852-byte DLAT · modality toggle · canonical calibration',
                style: TextStyle(fontSize: 13.5, color: Cozy.dimGray)),
            const SizedBox(height: 14),
            TextField(
              controller: _nameController,
              style: const TextStyle(
                  fontFamily: Cozy.monoFamily, fontSize: 14),
              decoration: InputDecoration(
                labelText: 'FRUIT NAME',
                prefixIcon:
                    Icon(Icons.local_florist_rounded, color: Cozy.matcha),
                filled: true,
                fillColor: Colors.white.withValues(alpha: 0.03),
                border: OutlineInputBorder(
                    borderRadius: BorderRadius.circular(16)),
              ),
            ),
            const SizedBox(height: 14),
            const SectionLabel(title: '// MODALITY'),
            Row(
              children: [
                for (var m = 0; m < 3; m++)
                  Expanded(
                    child: Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 2),
                      child: OutlinedButton(
                        onPressed: () =>
                            setState(() => _modality = m),
                        style: OutlinedButton.styleFrom(
                          backgroundColor: _modality == m
                              ? Cozy.matcha.withValues(alpha: 0.15)
                              : Colors.transparent,
                          side: BorderSide(
                              color: _modality == m
                                  ? Cozy.matcha
                                  : Colors.white.withValues(alpha: 0.08)),
                        ),
                        child: Text(
                          DualModelWire.modalityLabels[m],
                          style: TextStyle(
                              fontSize: 12,
                              fontWeight: FontWeight.bold,
                              color: _modality == m
                                  ? Cozy.matcha
                                  : Cozy.oatmeal),
                        ),
                      ),
                    ),
                  ),
              ],
            ),
            const SizedBox(height: 14),
            const SectionLabel(title: '// ENABLED CLASSES'),
            Text('only classes with saved data train; disabled score −100000',
                style: TextStyle(fontSize: 13.5, color: Cozy.dimGray)),
            const SizedBox(height: 8),
            for (var c = 0; c < BleProtocol.classLabels.length; c++)
              _classToggle(c),
          ],
        ),
      ),
    );
  }

  Widget _classToggle(int c) {
    final on = ((_mask >> c) & 1) == 1;
    final label = BleProtocol.classLabels[c];
    final color = Cozy.accents[c % Cozy.accents.length];
    return Padding(
      padding: const EdgeInsets.only(bottom: 4),
      child: GestureDetector(
        onTap: () => setState(
            () => _mask = on ? (_mask & ~(1 << c)) : (_mask | (1 << c))),
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 9),
          decoration: BoxDecoration(
            color: on
                ? color.withValues(alpha: 0.10)
                : Colors.white.withValues(alpha: 0.03),
            borderRadius: BorderRadius.circular(12),
            border: Border.all(
                color: on
                    ? color.withValues(alpha: 0.45)
                    : Colors.white.withValues(alpha: 0.06)),
          ),
          child: Row(children: [
            Icon(on ? Icons.check_circle : Icons.circle_outlined,
                size: 17, color: on ? color : Cozy.dimGray),
            const SizedBox(width: 10),
            Expanded(
              child: Text(label.replaceAll('_', ' ').toLowerCase(),
                  style: TextStyle(
                      fontFamily: Cozy.monoFamily,
                      fontSize: 15.5,
                      fontWeight: FontWeight.bold,
                      color: on ? Cozy.oatmeal : Cozy.dimGray)),
            ),
            Text('${_repo.tapCountOf(label)} taps',
                style: TextStyle(
                    fontSize: 12.5,
                    fontWeight: FontWeight.bold,
                    color: on ? Cozy.warmGray : Cozy.dimGray)),
          ]),
        ),
      ),
    );
  }

  /// Per-class data card: record more, add from saved, edit managed sessions.
  Widget _classCard(int c) {
    final label = BleProtocol.classLabels[c];
    final color = Cozy.accents[c % Cozy.accents.length];
    final sessions = _repo.sessionsFor(label);
    return Card(
      margin: EdgeInsets.zero,
      elevation: 0,
      color: const Color(0xFF1B1D22),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(16),
        side: BorderSide(color: color.withValues(alpha: 0.4), width: 1.2),
      ),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            Container(
              width: 11, height: 11,
              decoration: BoxDecoration(color: color, shape: BoxShape.circle),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: Text(label.replaceAll('_', ' ').toUpperCase(),
                  style: const TextStyle(
                      fontFamily: Cozy.monoFamily,
                      fontSize: 15,
                      fontWeight: FontWeight.w800,
                      color: Cozy.oatmeal)),
            ),
            Text(
                '${_repo.tapCountOf(label)} taps · ${sessions.length} '
                'session${sessions.length == 1 ? '' : 's'}',
                style: TextStyle(
                    fontSize: 12.5, fontWeight: FontWeight.bold, color: color)),
          ]),
          const SizedBox(height: 10),
          Row(children: [
            OutlinedButton.icon(
              style: OutlinedButton.styleFrom(
                foregroundColor: color,
                side: BorderSide(color: color.withValues(alpha: 0.5)),
                padding:
                    const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
              ),
              onPressed: () => _record(label),
              icon: const Icon(Icons.add_link_rounded, size: 15),
              label: const Text('RECORD',
                  style: TextStyle(fontWeight: FontWeight.bold, fontSize: 13)),
            ),
            const SizedBox(width: 8),
            TextButton.icon(
              style: TextButton.styleFrom(foregroundColor: Cozy.chamomile),
              onPressed: () => _addSaved(label),
              icon: const Icon(Icons.library_add_rounded, size: 15),
              label: const Text('ADD SAVED',
                  style: TextStyle(fontSize: 13)),
            ),
            if (sessions.isNotEmpty) ...[
              const SizedBox(width: 8),
              TextButton.icon(
                style: TextButton.styleFrom(foregroundColor: Cozy.duskBlue),
                onPressed: () => _editSession(label, sessions.first),
                icon: const Icon(Icons.edit_outlined, size: 15),
                label: const Text('EDIT',
                    style: TextStyle(fontSize: 13)),
              ),
            ],
          ]),
          if (sessions.isNotEmpty) ...[
            const SizedBox(height: 6),
            for (final s in sessions)
              Padding(
                padding: const EdgeInsets.only(bottom: 2),
                child: Row(children: [
                  const Icon(Icons.storage_rounded,
                      size: 14, color: Cozy.duskBlue),
                  const SizedBox(width: 6),
                  Expanded(
                    child: Text('${s.fruit} · ${s.tapCount} taps',
                        overflow: TextOverflow.ellipsis,
                        style: const TextStyle(
                            fontSize: 12.5, color: Cozy.warmGray)),
                  ),
                  GestureDetector(
                    onTap: () => _removeSession(s),
                    child: const Icon(Icons.close_rounded,
                        size: 15, color: Cozy.dimGray),
                  ),
                ]),
              ),
          ],
        ]),
      ),
    );
  }

  Widget _calibrationCard() {
    return FrostedBox(
      borderRadius: BorderRadius.circular(20),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          const SectionLabel(title: '// CALIBRATION (latest training controls)'),
          const SizedBox(height: 4),
          Text('mirrors tests/modelexp/build_dual_model_bin.py defaults',
              style: TextStyle(fontSize: 13.5, color: Cozy.dimGray)),
          const SizedBox(height: 12),
          _fieldRow(_r0SpecCtrl, 'r0_SPEC', 'sigmoid midpoint', Cozy.duskBlue),
          const SizedBox(height: 10),
          _fieldRow(_alphaCtrl, 'ALPHA', 'sigmoid slope', Cozy.heatherPink),
          const SizedBox(height: 10),
          _fieldRow(_minPeakCtrl, 'MIN_PEAK_ADC', 'tap-quality floor',
              Cozy.chamomile),
          const SizedBox(height: 10),
          if (_modality == 2) ...[
            _weightSlider(),
            const SizedBox(height: 12),
          ],
          const SectionLabel(title: '// STFT FEATURE MODE'),
          const SizedBox(height: 4),
          Row(
            children: [
              for (final (v, l) in [(0, 'LEGACY'), (1, 'STFT T1')])
                Expanded(
                  child: Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 2),
                    child: OutlinedButton(
                      onPressed: () => setState(() => _featMode = v),
                      style: OutlinedButton.styleFrom(
                        backgroundColor: _featMode == v
                            ? Cozy.matcha.withValues(alpha: 0.15)
                            : Colors.transparent,
                        side: BorderSide(
                            color: _featMode == v
                                ? Cozy.matcha
                                : Colors.white.withValues(alpha: 0.08)),
                      ),
                      child: Text(l,
                          style: TextStyle(
                              fontSize: 12,
                              fontWeight: FontWeight.bold,
                              color: _featMode == v
                                  ? Cozy.matcha
                                  : Cozy.oatmeal)),
                    ),
                  ),
                ),
            ],
          ),
          if (_featMode == 1) ...[
            const SizedBox(height: 10),
            _fieldRow(_stftPriorCtrl, 'STFT_T1_PRIOR', 'T1 decision midpoint',
                Cozy.mintCyan),
          ],
        ]),
      ),
    );
  }

  Widget _fieldRow(TextEditingController ctrl, String label, String hint,
      Color color) {
    return Row(children: [
      SizedBox(
        width: 150,
        child: Text(label,
            style: TextStyle(
                fontSize: 12,
                fontWeight: FontWeight.bold,
                letterSpacing: 0.6,
                color: color)),
      ),
      Expanded(
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          TextField(
            controller: ctrl,
            keyboardType: const TextInputType.numberWithOptions(
                decimal: true, signed: true),
            style: const TextStyle(fontFamily: Cozy.monoFamily, fontSize: 14),
            decoration: InputDecoration(
              isDense: true,
              hintText: hint,
              hintStyle: TextStyle(fontSize: 13, color: Cozy.dimGray),
              filled: true,
              fillColor: Colors.white.withValues(alpha: 0.03),
              border: OutlineInputBorder(
                  borderRadius: BorderRadius.circular(12)),
            ),
          ),
        ]),
      ),
    ]);
  }

  Widget _weightSlider() {
    return Row(children: [
      const Icon(Icons.balance_rounded, size: 16, color: Cozy.duskBlue),
      const SizedBox(width: 8),
      Expanded(
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text('DUAL WEIGHTS (w_acoustic)',
              style: TextStyle(
                  fontSize: 12,
                  fontWeight: FontWeight.bold,
                  letterSpacing: 0.6,
                  color: Cozy.duskBlue)),
          Text('w_optics = ${_wOptics.toStringAsFixed(2)} — INV d\' consensus '
              'uses 0.30 / 0.70',
              style: TextStyle(fontSize: 12.5, color: Cozy.dimGray)),
        ]),
      ),
      Expanded(
        child: SliderTheme(
          data: SliderTheme.of(context).copyWith(
            trackHeight: 5,
            activeTrackColor: Cozy.duskBlue.withValues(alpha: 0.85),
            inactiveTrackColor: Colors.white.withValues(alpha: 0.08),
            thumbColor: Cozy.duskBlue,
            overlayColor: Cozy.duskBlue.withValues(alpha: 0.15),
          ),
          child: Slider(
            value: _wAcoustic,
            min: 0.0,
            max: 1.0,
            divisions: 40,
            onChanged: (v) => setState(() => _wAcoustic = v),
          ),
        ),
      ),
      SizedBox(
        width: 40,
        child: Text('${_wAcoustic.toStringAsFixed(2)}',
            textAlign: TextAlign.right,
            style: TextStyle(
                fontSize: 14, fontWeight: FontWeight.bold, color: Cozy.duskBlue)),
      ),
    ]);
  }
}