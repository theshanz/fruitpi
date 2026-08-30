import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/cozy_palette.dart';
import '../core/model_vault.dart';
import '../core/rules_model32d.dart';
import '../services/ble_service.dart';
import '../widgets/frosted.dart';
import 'rule_builder_screen.dart';

/// Popup offering the different ways to add a fruit model:
///   • Rule Builder  — collectorrr-style knobs, built in-app
///   • Paste .bin    — base64/hex of a 616-byte Fruit28D blob
Future<void> showAddModelFlow(BuildContext context, BleService ble) {
  return showModalBottomSheet(
    context: context,
    showDragHandle: true,
    backgroundColor: Cozy.espresso.withValues(alpha: 0.97),
    builder: (ctx) => SafeArea(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(20, 0, 20, 24),
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          const Align(
            alignment: Alignment.centerLeft,
            child: SectionLabel(title: '// ENROLL NEW FRUIT MODEL'),
          ),
          const SizedBox(height: 14),
          ListTile(
            shape:
                RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
            leading: const Icon(Icons.tune_rounded, color: Cozy.matcha),
            title: const Text('RULE BUILDER',
                style: TextStyle(fontSize: 13, fontWeight: FontWeight.bold)),
            subtitle: Text('knobs per ripeness class · built on-device',
                style: TextStyle(fontSize: 15, color: Cozy.dimGray)),
            trailing: const Icon(Icons.chevron_right_rounded),
            onTap: () {
              Navigator.pop(ctx);
              Navigator.push(
                ctx,
                MaterialPageRoute(
                    builder: (_) => RuleBuilderScreen(bleService: ble)),
              );
            },
          ),
          const SizedBox(height: 6),
          ListTile(
            shape:
                RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
            leading: const Icon(Icons.data_object_rounded, color: Cozy.chamomile),
            title: const Text('PASTE .BIN',
                style: TextStyle(fontSize: 13, fontWeight: FontWeight.bold)),
            subtitle: Text('base64/hex blob from rules_engine.py',
                style: TextStyle(fontSize: 15, color: Cozy.dimGray)),
            trailing: const Icon(Icons.chevron_right_rounded),
            onTap: () async {
              Navigator.pop(ctx);
              final bin = await showPasteBinDialog(ctx);
              if (bin != null) await uploadBinWithProgress(ctx, ble, bin);
            },
          ),
        ]),
      ),
    ),
  );
}

// ── paste dialog ─────────────────────────────────────────────────────
Future<Uint8List?> showPasteBinDialog(BuildContext ctx) {
  final ctrl = TextEditingController();
  return showDialog<Uint8List>(
    context: ctx,
    builder: (dctx) => AlertDialog(
      backgroundColor: Cozy.surfaceCard,
      title: const Text('Paste model .bin',
          style: TextStyle(fontFamily: Cozy.monoFamily, fontSize: 15)),
      content: Column(mainAxisSize: MainAxisSize.min, children: [
        Text(
            'Export from the fruit-profile engine as base64 or hex.\n'
            'Accepted: ${RulesModel32D.wireBytes} B (legacy ${RulesModel32D.wireBytesLegacy} B).',
            style: TextStyle(fontSize: 15, color: Cozy.warmGray)),
        const SizedBox(height: 12),
        TextField(
          controller: ctrl,
          maxLines: 6,
          autofocus: true,
          style: const TextStyle(fontFamily: Cozy.monoFamily, fontSize: 15),
          decoration: InputDecoration(
              border:
                  OutlineInputBorder(borderRadius: BorderRadius.circular(12)),
              hintText: 'AAECAwQFBgc=',
              hintStyle: TextStyle(color: Cozy.dimGray)),
        ),
      ]),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(dctx), child: const Text('CANCEL')),
        FilledButton.icon(
          style: FilledButton.styleFrom(
              backgroundColor: Cozy.matcha, foregroundColor: Cozy.deepBg),
          icon: const Icon(Icons.paste_rounded, size: 16),
          label: const Text('PASTE & CHECK'),
          onPressed: () async {
            final text = ctrl.text.trim().isNotEmpty
                ? ctrl.text.trim()
                : (await Clipboard.getData(Clipboard.kTextPlain))?.text ?? '';
            final bin = RulesModel32D.tryParseBinText(text);
            if (!dctx.mounted) return;
            if (bin == null) {
              ScaffoldMessenger.of(dctx).showSnackBar(const SnackBar(
                  behavior: SnackBarBehavior.floating,
                  content: Text(
                      'Invalid blob — need 852/848-byte base64 or hex')));
              return;
            }
            Navigator.pop(dctx, bin);
          },
        ),
      ],
    ),
  );
}

// ── upload with frosted progress overlay ─────────────────────────────
Future<void> uploadBinWithProgress(
    BuildContext outerCtx, BleService ble, Uint8List bin) async {
  final notifier = ValueNotifier(0.0);

  showDialog(
    context: outerCtx,
    barrierDismissible: false,
    builder: (dctx) => PopScope(
      canPop: false,
      child: AlertDialog(
        backgroundColor: Cozy.surfaceCard,
        title: Text('UPLOADING "${RulesModel32D.binName(bin).toUpperCase()}"',
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

  final ok =
      await ble.uploadModelBin(bin, onProgress: (v) => notifier.value = v);
  if (ok) {
    ModelVault.put(bin);
    ble.refreshModels(); // status-driven refresh also covers this
  }

  if (outerCtx.mounted) {
    final nav = Navigator.of(outerCtx, rootNavigator: true);
    if (nav.canPop()) nav.pop();
  }
  if (!outerCtx.mounted) return;
  ScaffoldMessenger.of(outerCtx).showSnackBar(SnackBar(
    behavior: SnackBarBehavior.floating,
    backgroundColor: ok ? Colors.green.shade800 : Colors.red.shade800,
    content: Text(ok
        ? 'MODEL "${RulesModel32D.binName(bin)}" SAVED TO FLASH'
        : 'UPLOAD FAILED'),
  ));
}
