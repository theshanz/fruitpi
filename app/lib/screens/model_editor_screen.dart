import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/cozy_palette.dart';
import '../core/protocol.dart' show BleProtocol;
import '../core/rules_model32d.dart';
import '../services/ble_service.dart';
import '../widgets/frosted.dart';

/// Edits an existing 32-D model .bin — rename, enable/disable classes — then
/// re-uploads (same name = NVS overwrite). Note: the 32-D model uses
/// unit-normalized cosine prototypes, so there is no boundary-sharpness knob
/// (scaling a constant per-class bias cannot change the argmax).
class ModelEditorScreen extends StatefulWidget {
  final BleService bleService;
  final Uint8List originalBin;
  const ModelEditorScreen(
      {super.key, required this.bleService, required this.originalBin});

  @override
  State<ModelEditorScreen> createState() => _ModelEditorScreenState();
}

class _ModelEditorScreenState extends State<ModelEditorScreen> {
  late TextEditingController _nameCtrl;
  late int _mask;

  bool get _isLegacy => widget.originalBin.length < BleProtocol.modelWireBytes;

  @override
  void initState() {
    super.initState();
    _nameCtrl =
        TextEditingController(text: RulesModel32D.binName(widget.originalBin));
    _mask = RulesModel32D.maskOf(widget.originalBin);
  }

  Uint8List get _edited {
    var b = widget.originalBin;
    if (!_isLegacy) {
      b = RulesModel32D.withMask(b, _mask);
    }
    return RulesModel32D.withName(b, _nameCtrl.text.trim());
  }

  Set<int> get _enabledClasses => {
        for (var c = 0; c < RulesModel32D.classLabels.length; c++)
          if (((_mask >> c) & 1) == 1) c
      };

  Future<void> _saveAndUpload() async {
    final name = _nameCtrl.text.trim();
    if (name.isEmpty) {
      _toast('name cannot be empty');
      return;
    }
    if (_enabledClasses.isEmpty) {
      _toast('at least one class must stay enabled');
      return;
    }
    final bin = _edited;

    if (!widget.bleService.isConnected) {
      await Clipboard.setData(
          ClipboardData(text: RulesModel32D.toBase64(bin)));
      _toast('not connected — edited .bin copied to clipboard');
      return;
    }

    final notifier = ValueNotifier(0.0);
    showDialog(
      context: context,
      barrierDismissible: false,
      builder: (dctx) => PopScope(
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
            Text('overwriting flash entry…',
                style: TextStyle(fontSize: 15, color: Cozy.warmGray)),
          ]),
        ),
      ),
    );

    final ok = await widget.bleService.uploadModelBin(bin,
        onProgress: (v) => notifier.value = v);

    if (mounted) {
      final nav = Navigator.of(context, rootNavigator: true);
      if (nav.canPop()) nav.pop();
    }
    if (!mounted) return;
    _toast(ok ? 'model updated in flash!' : 'upload failed');
    if (ok) Navigator.pop(context, bin);
  }

  void _toast(String msg) {
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        behavior: SnackBarBehavior.floating,
        duration: const Duration(seconds: 3),
        content: Text(msg)));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Edit Model')),
      body: SafeArea(
        child: ListView(padding: const EdgeInsets.all(16), children: [
          FrostedBox(
            borderRadius: BorderRadius.circular(22),
            child:
                Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              const SectionLabel(title: '// IDENTITY'),
              const SizedBox(height: 12),
              TextField(
                controller: _nameCtrl,
                maxLength: 31,
                style:
                    const TextStyle(fontFamily: Cozy.monoFamily, fontSize: 13),
                decoration: InputDecoration(
                  labelText: 'FRUIT NAME',
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
              ),
              if (_isLegacy)
                Padding(
                  padding: const EdgeInsets.only(top: 6),
                  child: Text(
                      'legacy ${widget.originalBin.length}-byte model — '
                      'class editing unavailable',
                      style: TextStyle(fontSize: 14, color: Cozy.chamomile)),
                ),
              const SizedBox(height: 14),

              const SectionLabel(title: '// ENABLED CLASSES'),
              Text('disabled classes score −100000 on device',
                  style: TextStyle(fontSize: 13.5, color: Cozy.dimGray)),
              const SizedBox(height: 8),
              for (var c = 0; c < RulesModel32D.classLabels.length; c++)
                _classToggle(c),
            ]),
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
            onPressed: _saveAndUpload,
            icon: const Icon(Icons.save_rounded),
            label: const Text('SAVE & OVERWRITE ON DEVICE',
                style:
                    TextStyle(fontWeight: FontWeight.bold, letterSpacing: 0.8)),
          ),
          const SizedBox(height: 24),
        ]),
      ),
    );
  }

  Widget _classToggle(int c) {
    final on = ((_mask >> c) & 1) == 1;
    final label = RulesModel32D.classLabels[c];
    final color = Cozy.accents[c % Cozy.accents.length];
    return Padding(
      padding: const EdgeInsets.only(bottom: 4),
      child: GestureDetector(
        onTap: () =>
            setState(() => _mask = on ? (_mask & ~(1 << c)) : (_mask | (1 << c))),
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
          ]),
        ),
      ),
    );
  }
}
