import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/cozy_palette.dart';
import '../core/dual_model_wire.dart';
import '../core/protocol.dart';
import '../core/training_repository.dart';
import '../services/ble_service.dart';
import '../widgets/frosted.dart';
import 'data_collection_screen.dart';

/// Dual-Latent Rule Builder — category flow exactly like 32-D builder,
/// but uses dual-latent wire format (modality toggle + fixed weights).
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

  bool _isDirty = false;

  @override
  void initState() {
    super.initState();
    _mask = 0x1F;
    _modality = 2; // DUAL
    _nameController = TextEditingController(text: 'Mango Dual');
  }

  Set<int> get _enabled => {
        for (var c = 0; c < BleProtocol.classLabels.length; c++)
          if (((_mask >> c) & 1) == 1) c
      };

  Future<void> _upload() async {
    final wire = _repo.buildDualWire(
      fruitName: _nameController.text.trim().isEmpty ? 'Fruit' : _nameController.text.trim(),
      category: 'PERFECTLY_RIPE',
      modalityLabel: _modality == 0 ? 'ACOUSTIC' : (_modality == 1 ? 'OPTICS' : 'DUAL'),
      activeMask: _mask,
    );
    if (wire == null) {
      _toast('No session data for training');
      return;
    }
    if (!widget.bleService.isConnected) {
      await Clipboard.setData(ClipboardData(text: DualModelWire.binName(wire)));
      _toast('Not connected — wire name copied');
      return;
    }
    final ok = await widget.bleService.uploadModelBin(wire);
    _toast(ok ? 'DUAL MODEL UPLOADED' : 'UPLOAD FAILED');
  }

  void _toast(String msg) {
    ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
        behavior: SnackBarBehavior.floating,
        duration: Duration(seconds: 3),
        content: Text('')));
    // Use a quick snackbar message approach
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        behavior: SnackBarBehavior.floating,
        content: Text(msg),
    ));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Dual-Latent Builder')),
      body: SafeArea(
        child: ListView(padding: const EdgeInsets.all(16), children: [
          FrostedBox(
            borderRadius: BorderRadius.circular(22),
            child: Padding(
              padding: const EdgeInsets.all(14),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const SectionLabel(title: '// DUAL-LATENT MODEL'),
                  Text('852-byte DLAT · toggle modality · fixed weights',
                      style: TextStyle(fontSize: 13.5, color: Cozy.dimGray)),
                  const SizedBox(height: 14),
                  TextField(
                    controller: _nameController,
                    style: const TextStyle(fontFamily: Cozy.monoFamily, fontSize: 14),
                    decoration: InputDecoration(
                      labelText: 'FRUIT NAME',
                      prefixIcon: Icon(Icons.local_florist_rounded, color: Cozy.matcha),
                      filled: true,
                      fillColor: Colors.white.withValues(alpha: 0.03),
                      border: OutlineInputBorder(borderRadius: BorderRadius.circular(16)),
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
                              onPressed: () => setState(() => _modality = m),
                              style: OutlinedButton.styleFrom(
                                backgroundColor: _modality == m ? Cozy.matcha.withValues(alpha: 0.15) : Colors.transparent,
                                side: BorderSide(color: _modality == m ? Cozy.matcha : Colors.white.withValues(alpha: 0.08)),
                              ),
                              child: Text(
                                DualModelWire.modalityLabels[m],
                                style: TextStyle(fontSize: 12, fontWeight: FontWeight.bold, color: _modality == m ? Cozy.matcha : Cozy.oatmeal),
                              ),
                            ),
                          ),
                        )
                    ],
                  ),
                  const SizedBox(height: 14),
                  const SectionLabel(title: '// ENABLED CLASSES'),
                  Text('Disabled classes score −100000 on device',
                      style: TextStyle(fontSize: 13.5, color: Cozy.dimGray)),
                  const SizedBox(height: 8),
                  for (var c = 0; c < BleProtocol.classLabels.length; c++)
                    _classRow(c),
                  const SizedBox(height: 12),
                  FilledButton.icon(
                    style: FilledButton.styleFrom(
                      minimumSize: Size.fromHeight(54),
                      backgroundColor: Cozy.matcha,
                      foregroundColor: Cozy.deepBg,
                      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
                    ),
                    onPressed: _upload,
                    icon: Icon(Icons.save_rounded),
                    label: Text('TRAIN & UPLOAD DUAL MODEL',
                        style: TextStyle(fontWeight: FontWeight.bold, letterSpacing: 0.8)),
                  ),
                ],
              ),
            ),
          ),
        ]),
      ),
    );
  }

  Widget _classRow(int c) {
    final on = ((_mask >> c) & 1) == 1;
    final label = BleProtocol.classLabels[c];
    final color = Cozy.accents[c % Cozy.accents.length];
    return Padding(
      padding: EdgeInsets.only(bottom: 4),
      child: Container(
        padding: EdgeInsets.symmetric(horizontal: 12, vertical: 9),
        decoration: BoxDecoration(
          color: on ? color.withValues(alpha: 0.10) : Colors.white.withValues(alpha: 0.03),
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: on ? color.withValues(alpha: 0.45) : Colors.white.withValues(alpha: 0.06)),
        ),
        child: Row(children: [
          Icon(on ? Icons.check_circle : Icons.circle_outlined, size: 17, color: on ? color : Cozy.dimGray),
          SizedBox(width: 10),
          Expanded(child: Text(label.replaceAll('_', ' ').toLowerCase(), style: TextStyle(fontFamily: Cozy.monoFamily, fontSize: 15.5, fontWeight: FontWeight.bold, color: on ? Cozy.oatmeal : Cozy.dimGray))),
          IconButton(
            visualDensity: VisualDensity.compact,
            icon: Icon(Icons.hdr_weak_rounded, size: 18, color: Cozy.chamomile),
            tooltip: 'Collect data for $label',
            onPressed: () {
              Navigator.push(context, MaterialPageRoute(builder: (_) => DataCollectionScreen(
                bleService: widget.bleService,
                category: label,
                fruitName: _nameController.text.trim().isEmpty ? 'Fruit' : _nameController.text.trim(),
              )));
            },
          ),
        ]),
      ),
    );
  }
}
