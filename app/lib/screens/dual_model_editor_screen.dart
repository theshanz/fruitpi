import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/cozy_palette.dart';
import '../core/dual_model_wire.dart';
import '../core/model_vault.dart';
import '../core/protocol.dart';
import '../services/ble_service.dart';
import '../widgets/frosted.dart';
import 'data_collection_screen.dart';

/// New dual-latent model editor — 852-byte DLAT wire.
/// Toggle for modality (ACOUSTIC / OPTICS / DUAL) + category switches.
/// No weight sliders (weights fixed by mode).
class DualModelEditorScreen extends StatefulWidget {
  final BleService bleService;
  final Uint8List originalBin;
  const DualModelEditorScreen(
      {super.key, required this.bleService, required this.originalBin});

  @override
  State<DualModelEditorScreen> createState() => _DualModelEditorScreenState();
}

class _DualModelEditorScreenState extends State<DualModelEditorScreen> {
  late TextEditingController _nameCtrl;
  late int _mask;
  late int _modality;

  bool get _isValid => DualModelWire.isValid(widget.originalBin);

  @override
  void initState() {
    super.initState();
    // Try loading saved data from vault (like 32D editor)
    final saved = ModelVault.get(DualModelWire.binName(widget.originalBin));
    final binToEdit = saved ?? widget.originalBin;
    _nameCtrl = TextEditingController(
        text: DualModelWire.binName(binToEdit));
    _mask = DualModelWire.maskOf(binToEdit);
    _modality = DualModelWire.modalityOf(binToEdit);
  }

  Uint8List get _edited {
    var b = DualModelWire.withModality(
        DualModelWire.withMask(
            DualModelWire.withName(widget.originalBin, _nameCtrl.text.trim()),
            _mask),
        _modality);
    return b;
  }

  Set<int> get _enabledClasses => {
        for (var c = 0; c < BleProtocol.classLabels.length; c++)
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
      ModelVault.put(bin);
      await Clipboard.setData(
          ClipboardData(text: DualModelWire.binName(bin)));
      _toast('not connected — edited .bin copied');
      return;
    }

    final ok = await widget.bleService.uploadModelBin(bin);
    if (ok) ModelVault.put(bin);
    if (!mounted) return;
    _toast(ok ? 'dual model updated!' : 'upload failed');
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
      appBar: AppBar(title: const Text('Edit Dual-Latent Model')),
      body: SafeArea(
        child: ListView(padding: const EdgeInsets.all(16), children: [
          FrostedBox(
            borderRadius: BorderRadius.circular(22),
            child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const SectionLabel(title: '// DUAL-LATENT MODEL'),
                  const SizedBox(height: 6),
                  Text('852-byte DLAT wire · toggle modality · no sliders · data feeds into wire',
                      style: TextStyle(fontSize: 13.5, color: Cozy.dimGray)),
                  const SizedBox(height: 12),

                  // Name
                  TextField(
                    controller: _nameCtrl,
                    maxLength: 31,
                    style: const TextStyle(
                        fontFamily: Cozy.monoFamily, fontSize: 13),
                    decoration: InputDecoration(
                      labelText: 'FRUIT NAME',
                      labelStyle: TextStyle(fontSize: 15, color: Cozy.dimGray),
                      prefixIcon: const Icon(Icons.local_florist_rounded,
                          color: Cozy.matcha),
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
                  // Data inspection preview (matches 32d collection preview)
                  FrostedBox(
                    borderRadius: BorderRadius.circular(16),
                    child: Padding(
                      padding: const EdgeInsets.all(12),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Row(children: [
                            const Icon(Icons.visibility_rounded,
                                size: 16, color: Cozy.chamomile),
                            const SizedBox(width: 8),
                            const Text('DATA INSPECTION',
                                style: TextStyle(
                                    fontSize: 11,
                                    letterSpacing: 1,
                                    color: Cozy.dimGray)),
                          ]),
                          const SizedBox(height: 8),
                          Text('Wire: DLAT · ${DualModelWire.binName(widget.originalBin)} · ${widget.originalBin.length} B',
                              style: const TextStyle(
                                  fontSize: 13, color: Cozy.oatmeal)),
                          const SizedBox(height: 4),
                          Text('Modality: ${DualModelWire.modalityLabels[DualModelWire.modalityOf(widget.originalBin)]} · Mask: 0x${_mask.toRadixString(16).padLeft(2, '0').toUpperCase()}',
                              style: const TextStyle(
                                  fontSize: 12, color: Cozy.dimGray)),
                          const SizedBox(height: 4),
                          Text('Active classes: ${_enabledClasses.map((c) => BleProtocol.classLabels[c]).join(', ')}',
                              style: const TextStyle(
                                  fontSize: 11.5, color: Cozy.dimGray)),
                        ],
                      ),
                    ),
                  ),
                  const SizedBox(height: 14),

                  // Modality toggle (3-state, no sliders)
                  const SectionLabel(title: '// MODALITY (TOGGLE)'),
                  Text('ACOUSTIC / OPTICS / DUAL — weights fixed by mode',
                      style: TextStyle(fontSize: 13.5, color: Cozy.dimGray)),
                  const SizedBox(height: 8),
                  Row(
                    children: [
                      for (var m = 0; m < 3; m++)
                        Expanded(
                          child: Padding(
                            padding: const EdgeInsets.symmetric(horizontal: 2),
                            child: OutlinedButton(
                              onPressed: () => setState(() => _modality = m),
                              style: OutlinedButton.styleFrom(
                                backgroundColor: _modality == m
                                    ? Cozy.matcha.withValues(alpha: 0.15)
                                    : Colors.transparent,
                                side: BorderSide(
                                  color: _modality == m
                                      ? Cozy.matcha
                                      : Colors.white.withValues(alpha: 0.08),
                                ),
                              ),
                              child: Text(
                                DualModelWire.modalityLabels[m],
                                style: TextStyle(
                                  fontSize: 12,
                                  fontWeight: FontWeight.bold,
                                  color: _modality == m
                                      ? Cozy.matcha
                                      : Cozy.oatmeal,
                                ),
                              ),
                            ),
                          ),
                        )
                    ],
                  ),
                  const SizedBox(height: 14),

                  // Enabled classes
                  const SectionLabel(title: '// ENABLED CLASSES'),
                  Text('disabled classes score −100000 on device',
                      style: TextStyle(fontSize: 13.5, color: Cozy.dimGray)),
                  const SizedBox(height: 8),
                  for (var c = 0; c < BleProtocol.classLabels.length; c++)
                    _classToggle(c),
                ],
              ),
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
                style: TextStyle(
                    fontWeight: FontWeight.bold, letterSpacing: 0.8)),
          ),
          const SizedBox(height: 24),
        ]),
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
                color: on ? color.withValues(alpha: 0.45) : Colors.white.withValues(alpha: 0.06)),
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
            if (on)
              IconButton(
                visualDensity: VisualDensity.compact,
                icon: const Icon(Icons.hdr_weak_rounded,
                    size: 18, color: Cozy.chamomile),
                tooltip: 'Collect data for $label',
                onPressed: () {
                  Navigator.push(
                    context,
                    MaterialPageRoute(
                      builder: (_) => DataCollectionScreen(
                        bleService: widget.bleService,
                        category: label,
                        fruitName: _nameCtrl.text.trim().isEmpty
                            ? 'Fruit'
                            : _nameCtrl.text.trim(),
                      ),
                    ),
                  );
                },
              ),
          ]),
        ),
      ),
    );
  }
}
