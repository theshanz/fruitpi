import 'dart:io' show Platform;

import 'package:flutter/material.dart';

import '../core/cozy_palette.dart';
import '../core/training_repository.dart';
import '../widgets/frosted.dart';
import '../widgets/spectra_charts.dart';
import '../widgets/waveform_chart.dart';

/// Dataset / session manager.
///
/// Lists every saved session (all on-disk folders pulled into memory), shows
/// per-session details (tap count, hue count, volume, timestamp, assigned
/// category) and lets the user inspect individual taps, rename, delete, or
/// reassign a session to a different category.
class DatasetsScreen extends StatefulWidget {
  final TrainingRepository repo;
  final List<String> categoryOptions;
  const DatasetsScreen({
    super.key,
    required this.repo,
    this.categoryOptions = const [],
  });

  @override
  State<DatasetsScreen> createState() => _DatasetsScreenState();
}

class _DatasetsScreenState extends State<DatasetsScreen> {
  late final TrainingRepository _repo = widget.repo;
  bool _refreshing = false;

  List<String> get _options {
    final opts = <String>{...widget.categoryOptions};
    for (final s in _repo.allSessions) {
      opts.add(_repo.assignedCategory(s.id));
    }
    // natural sorted, current options first
    final known = widget.categoryOptions
        .where(opts.contains)
        .toList();
    final rest = (opts.toList()..removeWhere(known.contains))..sort();
    return [...known, ...rest];
  }

  Future<void> _refresh() async {
    setState(() => _refreshing = true);
    if (Platform.isLinux && _repo.pickedBaseDir == null) {
      await _repo.pickBaseDir();
    }
    await _repo.loadFromDisk();
    if (mounted) setState(() => _refreshing = false);
  }

  Future<void> _assign(SampleSession s) async {
    final cat = await showDialog<String>(
      context: context,
      builder: (ctx) => _AssignDialog(
        title: 'ASSIGN “${s.fruit}” TO CATEGORY',
        options: _options,
        current: _repo.assignedCategory(s.id),
        color: _colorFor(s),
      ),
    );
    if (cat != null && mounted) {
      await _repo.assignSession(s.id, cat);
    }
  }

  Future<void> _rename(SampleSession s) async {
    final ctrl = TextEditingController(text: s.fruit);
    final name = await showDialog<String>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: Cozy.surfaceCard,
        title: const Text('RENAME SESSION',
            style: TextStyle(fontFamily: Cozy.monoFamily, fontSize: 14)),
        content: TextField(
          controller: ctrl,
          autofocus: true,
          style: const TextStyle(fontSize: 16, color: Cozy.oatmeal),
          decoration: InputDecoration(
            labelText: 'FRUIT / SESSION NAME',
            labelStyle: const TextStyle(fontSize: 14, color: Cozy.dimGray),
            border: OutlineInputBorder(
                borderRadius: BorderRadius.circular(12)),
          ),
        ),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(ctx), child: const Text('CANCEL')),
          FilledButton(
              onPressed: () => Navigator.pop(ctx, ctrl.text.trim()),
              child: const Text('RENAME')),
        ],
      ),
    );
    if (name != null && name.isNotEmpty && mounted) {
      await _repo.renameSession(s.id, name);
    }
  }

  Future<void> _delete(SampleSession s) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: Cozy.surfaceCard,
        title: const Text('DELETE SESSION?',
            style: TextStyle(fontFamily: Cozy.monoFamily, fontSize: 14)),
        content: Text(
            'Delete ${s.fruit} (${s.tapCount} taps, ${s.hueCount} hues) and '
            'its on-disk folder? This cannot be undone.'),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(ctx, false),
              child: const Text('CANCEL')),
          FilledButton(
            style: FilledButton.styleFrom(backgroundColor: Cozy.roseError),
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('DELETE'),
          ),
        ],
      ),
    );
    if (ok ?? false) {
      await _repo.deleteSession(s.id);
    }
  }

  void _inspect(SampleSession s) {
    showModalBottomSheet<void>(
      context: context,
      backgroundColor: Cozy.surfaceCard,
      isScrollControlled: true,
      shape: const RoundedRectangleBorder(
          borderRadius: BorderRadius.vertical(top: Radius.circular(22))),
      builder: (sheet) => _SessionDetailSheet(repo: _repo, session: s),
    );
  }

  Color _colorFor(SampleSession s) {
    final idx = _options.indexOf(_repo.assignedCategory(s.id));
    return Cozy.accents[idx < 0 ? 0 : (idx % Cozy.accents.length)];
  }

  @override
  Widget build(BuildContext context) {
    final sessions = _repo.allSessions;
    return Scaffold(
      appBar: AppBar(
        title: const Text('Datasets'),
        actions: [
          TextButton.icon(
            onPressed: _refreshing ? null : _refresh,
            icon: _refreshing
                ? const SizedBox(
                    width: 16,
                    height: 16,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : const Icon(Icons.refresh_rounded, size: 18),
            label: const Text('LOAD FROM DISK',
                style: TextStyle(fontWeight: FontWeight.bold)),
          ),
        ],
      ),
      body: SafeArea(
        child: sessions.isEmpty
            ? const Center(
                child: Text(
                  'No datasets yet.\nLoad from disk or record a session.',
                  textAlign: TextAlign.center,
                  style: TextStyle(
                      fontSize: 15, color: Cozy.dimGray, height: 1.5),
                ),
              )
            : ListView(
                padding: const EdgeInsets.all(16),
                children: [
                  Padding(
                    padding: const EdgeInsets.only(bottom: 10),
                    child: Text(
                      '${sessions.length} saved session${sessions.length == 1 ? '' : 's'} · '
                      'assign each to a category to feed its model',
                      style: const TextStyle(fontSize: 13.5, color: Cozy.dimGray),
                    ),
                  ),
                  for (final s in sessions) _card(s),
                ],
              ),
      ),
    );
  }

  Widget _card(SampleSession s) {
    final color = _colorFor(s);
    final cat = _repo.assignedCategory(s.id);
    return Card(
      margin: const EdgeInsets.only(bottom: 10),
      elevation: 0,
      color: const Color(0xFF1B1D22),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(16),
        side: BorderSide(color: color.withValues(alpha: 0.4), width: 1.2),
      ),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(14, 12, 8, 12),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            Container(
              width: 11,
              height: 11,
              decoration: BoxDecoration(color: color, shape: BoxShape.circle),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: Text(s.fruit,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                      fontFamily: Cozy.monoFamily,
                      fontSize: 16,
                      fontWeight: FontWeight.w800,
                      color: Cozy.oatmeal)),
            ),
            _chip(cat, color),
          ]),
          const SizedBox(height: 8),
          Row(children: [
            _metric(Icons.graphic_eq_rounded, '${s.tapCount} taps'),
            const SizedBox(width: 12),
            _metric(Icons.colorize_rounded, '${s.hueCount} hues'),
            const SizedBox(width: 12),
            Expanded(
              child: Text(
                _fmtVol(s.aggregateHue?.volumeCm3),
                style: const TextStyle(fontSize: 13, color: Cozy.dimGray),
              ),
            ),
          ]),
          const SizedBox(height: 4),
          Text(_fmtWhen(s.timestamp),
              style: const TextStyle(fontSize: 12.5, color: Cozy.dimGray)),
          const SizedBox(height: 6),
          Row(children: [
            TextButton.icon(
              style: TextButton.styleFrom(foregroundColor: color),
              onPressed: () => _inspect(s),
              icon: const Icon(Icons.insert_chart_outlined_rounded, size: 16),
              label: const Text('INSPECT',
                  style: TextStyle(fontWeight: FontWeight.bold)),
            ),
            TextButton.icon(
              style: TextButton.styleFrom(foregroundColor: Cozy.chamomile),
              onPressed: () => _assign(s),
              icon: const Icon(Icons.settings_suggest_rounded, size: 16),
              label: const Text('ASSIGN',
                  style: TextStyle(fontWeight: FontWeight.bold)),
            ),
            TextButton.icon(
              style: TextButton.styleFrom(foregroundColor: Cozy.duskBlue),
              onPressed: () => _rename(s),
              icon: const Icon(Icons.drive_file_rename_outline_rounded, size: 16),
              label: const Text('RENAME',
                  style: TextStyle(fontWeight: FontWeight.bold)),
            ),
            TextButton.icon(
              style: TextButton.styleFrom(foregroundColor: Cozy.roseError),
              onPressed: () => _delete(s),
              icon: const Icon(Icons.delete_outline_rounded, size: 16),
              label: const Text('DELETE',
                  style: TextStyle(fontWeight: FontWeight.bold)),
            ),
          ]),
        ]),
      ),
    );
  }

  Widget _chip(String label, Color color) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
        decoration: BoxDecoration(
          color: color.withValues(alpha: 0.14),
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: color.withValues(alpha: 0.5)),
        ),
        child: Text(label.replaceAll('_', ' '),
            style: TextStyle(
                fontSize: 12,
                fontWeight: FontWeight.bold,
                color: color)),
      );

  Widget _metric(IconData icon, String label) => Row(children: [
        Icon(icon, size: 15, color: Cozy.warmGray),
        const SizedBox(width: 4),
        Text(label, style: const TextStyle(fontSize: 13, color: Cozy.warmGray)),
      ]);

  static String _fmtVol(double? v) =>
      v == null ? 'vol —' : 'vol ${v.toStringAsFixed(0)} cm³';

  static String _fmtWhen(DateTime t) =>
      '${t.year}-${t.month.toString().padLeft(2, '0')}-'
      '${t.day.toString().padLeft(2, '0')} · '
      '${t.hour.toString().padLeft(2, '0')}:'
      '${t.minute.toString().padLeft(2, '0')}';
}

/// Bottom sheet showing all taps + hue captures of one session, each with its
/// own raw waveform and FFT spectrum.
class _SessionDetailSheet extends StatefulWidget {
  final TrainingRepository repo;
  final SampleSession session;
  const _SessionDetailSheet({required this.repo, required this.session});

  @override
  State<_SessionDetailSheet> createState() => _SessionDetailSheetState();
}

class _SessionDetailSheetState extends State<_SessionDetailSheet> {
  int? _selTap;

  @override
  Widget build(BuildContext context) {
    final s = widget.session;
    final cat = widget.repo.assignedCategory(s.id);
    final idx = widget.repo.allSessions.indexWhere((x) => x.id == s.id);
    final color = Cozy.accents[idx < 0 ? 0 : (idx % Cozy.accents.length)];
    return DraggableScrollableSheet(
      initialChildSize: 0.9,
      maxChildSize: 0.98,
      minChildSize: 0.6,
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
            _chip(cat, color),
          ]),
          const SizedBox(height: 4),
          Text(
            '${s.tapCount} taps · ${s.hueCount} hues · '
            '${s.aggregateHue?.volumeCm3.toStringAsFixed(0) ?? '—'} cm³',
            style: const TextStyle(fontSize: 13, color: Cozy.dimGray),
          ),
          const SizedBox(height: 16),

          if (s.hues.isNotEmpty) ...[
            const SectionLabel(title: '// HUE CAPTURES'),
            const SizedBox(height: 6),
            for (var i = 0; i < s.hues.length; i++)
              Padding(
                padding: const EdgeInsets.only(bottom: 8),
                child: HueHistogramChart(
                  series: PlotSeries(s.fruit, color, s.hues[i].hueHistogram),
                  height: 100,
                ),
              ),
            const SizedBox(height: 10),
          ],

          const SectionLabel(title: '// TAPS (per-tap waveform + spectrum)'),
          const SizedBox(height: 6),
          for (var i = 0; i < s.taps.length; i++) _tapCard(s.taps[i], i, color),

          const SizedBox(height: 12),
          OutlinedButton.icon(
            style: OutlinedButton.styleFrom(
              foregroundColor: Cozy.dimGray,
              side: BorderSide(color: Colors.white.withValues(alpha: 0.1)),
            ),
            onPressed: () => Navigator.pop(context),
            icon: const Icon(Icons.close_rounded, size: 18),
            label: const Text('CLOSE',
                style: TextStyle(fontWeight: FontWeight.bold)),
          ),
        ],
      ),
    );
  }

  Widget _tapCard(TapRecord t, int i, Color color) {
    final sel = _selTap == i;
    return Card(
      margin: const EdgeInsets.only(bottom: 8),
      elevation: 0,
      color: const Color(0xFF15171B),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: BorderSide(
            color: sel ? color.withValues(alpha: 0.7) : Colors.white.withValues(alpha: 0.06)),
      ),
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: () => setState(() => _selTap = sel ? null : i),
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Row(children: [
              Icon(Icons.graphic_eq_rounded, size: 15, color: color),
              const SizedBox(width: 6),
              Text('TAP ${i + 1}',
                  style: const TextStyle(
                      fontFamily: Cozy.monoFamily,
                      fontSize: 13,
                      fontWeight: FontWeight.w800,
                      color: Cozy.oatmeal)),
              const Spacer(),
              Icon(sel ? Icons.expand_less_rounded : Icons.expand_more_rounded,
                  size: 18, color: Cozy.dimGray),
            ]),
            const SizedBox(height: 10),
            const SectionLabel(title: '// RAW WAVEFORM (ADC)'),
            const SizedBox(height: 4),
            SizedBox(height: 110, child: WaveformChart(samples: t.waveform)),
            const SizedBox(height: 8),
            const SectionLabel(title: '// SPECTRUM (FFT magnitude)'),
            const SizedBox(height: 4),
            SizedBox(
                height: 120,
                child: SpectrumChart(
                    series: PlotSeries('t${i + 1}', color, t.spectrum))),
          ]),
        ),
      ),
    );
  }

  Widget _chip(String label, Color color) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
        decoration: BoxDecoration(
          color: color.withValues(alpha: 0.14),
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: color.withValues(alpha: 0.5)),
        ),
        child: Text(label.replaceAll('_', ' '),
            style: TextStyle(fontSize: 12, fontWeight: FontWeight.bold, color: color)),
      );
}

class _AssignDialog extends StatefulWidget {
  final String title;
  final List<String> options;
  final String current;
  final Color color;
  const _AssignDialog({
    required this.title,
    required this.options,
    required this.current,
    required this.color,
  });

  @override
  State<_AssignDialog> createState() => _AssignDialogState();
}

class _AssignDialogState extends State<_AssignDialog> {
  late String? _sel = widget.current.isNotEmpty ? widget.current : null;
  final _newCtrl = TextEditingController();

  @override
  void dispose() {
    _newCtrl.dispose();
    super.dispose();
  }

  void _ok() {
    final custom = _newCtrl.text.trim();
    if (custom.isNotEmpty) {
      Navigator.pop(context, custom);
    } else if (_sel != null) {
      Navigator.pop(context, _sel);
    }
  }

  @override
  Widget build(BuildContext context) {
    final options = widget.options;
    return AlertDialog(
      backgroundColor: Cozy.surfaceCard,
      title: Text(widget.title,
          style: const TextStyle(
              fontFamily: Cozy.monoFamily, fontSize: 14, color: Cozy.oatmeal)),
      content: SingleChildScrollView(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          RadioGroup<String>(
            groupValue: _sel,
            onChanged: (v) => setState(() => _sel = v),
            child: Column(mainAxisSize: MainAxisSize.min, children: [
              for (final o in options)
                RadioListTile<String>(
                  dense: true,
                  contentPadding: EdgeInsets.zero,
                  value: o,
                  activeColor: widget.color,
                  title: Text(o.replaceAll('_', ' '),
                      style: const TextStyle(fontSize: 15, color: Cozy.oatmeal)),
                ),
            ]),
          ),
          const SizedBox(height: 6),
          TextField(
            controller: _newCtrl,
            style: const TextStyle(fontSize: 15, color: Cozy.oatmeal),
            decoration: InputDecoration(
              labelText: 'OR NEW CATEGORY',
              labelStyle: const TextStyle(fontSize: 13, color: Cozy.dimGray),
              isDense: true,
              border: OutlineInputBorder(
                  borderRadius: BorderRadius.circular(10)),
            ),
            onChanged: (_) => setState(() {}),
          ),
        ]),
      ),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('CANCEL')),
        FilledButton(
          onPressed: (_sel == null && _newCtrl.text.trim().isEmpty)
              ? null
              : _ok,
          child: const Text('ASSIGN'),
        ),
      ],
    );
  }
}
