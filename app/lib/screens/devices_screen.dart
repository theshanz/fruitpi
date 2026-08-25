import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../core/cozy_palette.dart';
import '../core/protocol.dart';
import '../services/ble_service.dart';
import '../widgets/frosted.dart';

/// BLE discovery — scans continuously, surfaces FruitPi first (matched by
/// advertised service UUID *or* name), other devices visible for debugging.
class DevicesScreen extends StatefulWidget {
  final BleService bleService;
  const DevicesScreen({super.key, required this.bleService});

  @override
  State<DevicesScreen> createState() => _DevicesScreenState();
}

class _DevicesScreenState extends State<DevicesScreen> {
  bool _connecting = false;
  String _hint = '';

  bool get _scanning => FlutterBluePlus.isScanningNow;

  bool isFruitPi(ScanResult r) {
    final name = r.device.platformName.toLowerCase();
    if (name.contains('fruitipi') || name.contains('fruitpi')) return true;
    for (final u in r.advertisementData.serviceUuids) {
      if (u.str.toLowerCase() == BleProtocol.serviceUuid) return true;
    }
    return false;
  }

  Future<void> _startScan() async {
    final ok = await widget.bleService.requestPermissions();
    if (!ok && mounted) {
      setState(() => _hint =
          'permissions denied — grant Nearby devices + Location in system settings');
    }
    // continuous scan; stopped on pick/dispose/refresh
    FlutterBluePlus.startScan();
    if (mounted) setState(() => _hint = '');
  }

  Future<void> _pick(ScanResult r) async {
    FlutterBluePlus.stopScan();
    setState(() {
      _connecting = true;
      _hint = '';
    });
    final ok = await widget.bleService.connect(r.device);
    if (!mounted) return;
    setState(() => _connecting = false);
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      behavior: SnackBarBehavior.floating,
      backgroundColor: ok ? Colors.green.shade800 : Colors.red.shade800,
      content: Text(ok
          ? 'connected: ${r.device.platformName.isNotEmpty ? r.device.platformName : r.device.remoteId.str}'
          : 'connection failed — is another phone still bonded to it?'),
    ));
    if (ok) Navigator.pop(context);
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _startScan());
  }

  @override
  void dispose() {
    FlutterBluePlus.stopScan();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Available Devices'),
        actions: [
          IconButton(
            tooltip: _scanning ? 'restart scan' : 'scan',
            icon: Icon(_scanning ? Icons.pause_circle_outline_rounded : Icons.refresh),
            onPressed: () async {
              await FlutterBluePlus.stopScan();
              _startScan();
            },
          ),
        ],
      ),
      body: Stack(children: [
        StreamBuilder<List<ScanResult>>(
          stream: FlutterBluePlus.scanResults,
          builder: (context, snap) {
            final all = (snap.data ?? const <ScanResult>[])
                .toList()
              ..sort((a, b) => b.rssi.compareTo(a.rssi));
            final fruits = all.where(isFruitPi).toList();
            final others =
                all.where((r) => !isFruitPi(r)).take(8).toList();

            return ListView(
              padding: const EdgeInsets.all(14),
              children: [
                Row(children: [
                  const Expanded(
                      child: SectionLabel(title: '// FRUITPI DEVICES')),
                  AnimatedContainer(
                    duration: const Duration(milliseconds: 300),
                    width: 10,
                    height: 10,
                    decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        color: _scanning ? Cozy.matcha : Cozy.dimGray),
                  ),
                  const SizedBox(width: 6),
                  Text(_scanning ? 'SCANNING' : 'IDLE',
                      style: TextStyle(
                          fontSize: 10,
                          fontWeight: FontWeight.bold,
                          color: _scanning ? Cozy.matcha : Cozy.dimGray)),
                ]),
                const SizedBox(height: 10),

                if (_hint.isNotEmpty)
                  Padding(
                    padding: const EdgeInsets.only(bottom: 10),
                    child: Text(_hint,
                        style:
                            TextStyle(fontSize: 11, color: Cozy.chamomile)),
                  ),

                if (fruits.isEmpty)
                  FrostedBox(
                    borderRadius: BorderRadius.circular(20),
                    child: SizedBox(
                      width: double.infinity,
                      child: Padding(
                        padding: const EdgeInsets.symmetric(vertical: 22),
                        child: Column(children: [
                          if (!_scanning) ...[
                            Text('scan paused — tap ↻',
                                style: TextStyle(
                                    fontSize: 11, color: Cozy.warmGray)),
                          ] else ...[
                            Text('searching for "Fruitipi"…',
                                style: TextStyle(
                                    fontSize: 11, color: Cozy.dimGray)),
                            const SizedBox(height: 4),
                            Text(
                                '${all.length} BLE devices seen so far',
                                style: TextStyle(
                                    fontSize: 9.5, color: Cozy.dimGray)),
                          ],
                        ]),
                      ),
                    ),
                  )
                else
                  for (final r in fruits)
                    Padding(
                      padding: const EdgeInsets.only(bottom: 8),
                      child: _deviceCard(r, highlight: true),
                    ),

                const SizedBox(height: 18),
                const SectionLabel(title: '// OTHER BLE NEARBY (debug)'),
                const SizedBox(height: 8),
                if (others.isEmpty)
                  Text(_scanning ? 'listening…' : '—',
                      style: TextStyle(fontSize: 10, color: Cozy.dimGray))
                else
                  ...others.map((r) => ListTile(
                        dense: true,
                        visualDensity: VisualDensity.compact,
                        leading: Icon(Icons.bluetooth_disabled_rounded,
                            size: 16, color: Cozy.dimGray),
                        title: Text(
                            (r.device.platformName.isNotEmpty
                                    ? r.device.platformName
                                    : '(unnamed)'),
                            style: TextStyle(
                                fontSize: 11, color: Cozy.warmGray)),
                        subtitle: Text('${r.device.remoteId.str} · ${r.rssi} dBm',
                            style: TextStyle(
                                fontSize: 9, color: Cozy.dimGray)),
                        onTap: () => _pick(r), // allow manual try anyway
                      )),
                const SizedBox(height: 30),
              ],
            );
          },
        ),
        if (_connecting)
          Container(
            color: Colors.black54,
            child: const Center(child: CircularProgressIndicator()),
          ),
      ]),
    );
  }

  Widget _deviceCard(ScanResult r, {required bool highlight}) {
    final name = r.device.platformName.isNotEmpty
        ? r.device.platformName
        : 'FruitPi (${r.device.remoteId.str})';
    return GestureDetector(
      onTap: _connecting ? null : () => _pick(r),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 13),
        decoration: BoxDecoration(
          color: Cozy.matcha.withValues(alpha: 0.08),
          borderRadius: BorderRadius.circular(18),
          border:
              Border.all(color: Cozy.matcha.withValues(alpha: 0.45)),
        ),
        child: Row(children: [
          const Icon(Icons.bluetooth_rounded, color: Cozy.matcha, size: 20),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(name,
                      style: const TextStyle(
                          fontFamily: Cozy.monoFamily,
                          fontSize: 13,
                          fontWeight: FontWeight.bold,
                          color: Cozy.oatmeal)),
                  Text('${r.device.remoteId.str} · ${r.rssi} dBm',
                      style: TextStyle(fontSize: 9.5, color: Cozy.dimGray)),
                ]),
          ),
          const Icon(Icons.chevron_right_rounded,
              color: Cozy.matcha, size: 22),
        ]),
      ),
    );
  }
}
