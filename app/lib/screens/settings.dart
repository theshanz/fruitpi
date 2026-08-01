import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../services/ble_service.dart';

/// Device discovery screen - scan and pick an ESP32 to connect to.
class SettingsScreen extends StatefulWidget {
  final BleService bleService;
  const SettingsScreen({super.key, required this.bleService});

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  List<ScanResult> _devices = [];
  bool _isScanning = false;
  StreamSubscription<List<ScanResult>>? _scanSub;

  // Known ESP32 MAC for quick connect
  static const String _espMac = 'E0:72:A1:A7:D7:4D';

  @override
  void initState() {
    super.initState();
    _startScan();
  }

  void _startScan() async {
    setState(() {
      _devices = [];
      _isScanning = true;
    });

    _scanSub?.cancel();
    _scanSub = widget.bleService.scan().listen((results) {
      setState(() => _devices = results);
    });

    // Auto-stop scanning after timeout
    Future.delayed(const Duration(seconds: 8), () {
      if (mounted) setState(() => _isScanning = false);
    });
  }

  void _onDeviceTap(BluetoothDevice device) {
    widget.bleService.stopScan();
    Navigator.pop(context, device);
  }

  void _quickConnect() {
    // Connect directly by MAC address, bypassing scan
    final device = BluetoothDevice.fromId(_espMac);
    widget.bleService.stopScan();
    Navigator.pop(context, device);
  }

  @override
  void dispose() {
    _scanSub?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Available Devices'),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            onPressed: _isScanning ? null : _startScan,
          ),
        ],
      ),
      body: Column(
        children: [
          // Quick connect card
          Card(
            margin: const EdgeInsets.all(12),
            child: ListTile(
              leading: const Icon(Icons.bluetooth, color: Colors.green),
              title: const Text('FruitPi (ESP32)'),
              subtitle: Text(_espMac),
              trailing: const Icon(Icons.flash_on),
              onTap: _quickConnect,
            ),
          ),
          const Divider(),
          // Scan results
          Expanded(
            child: _devices.isEmpty
                ? Center(
                    child: _isScanning
                        ? const Column(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              CircularProgressIndicator(),
                              SizedBox(height: 16),
                              Text('Scanning...'),
                            ],
                          )
                        : const Text('No other devices found.'),
                  )
                : ListView.builder(
                    itemCount: _devices.length,
                    itemBuilder: (context, index) {
                      final result = _devices[index];
                      final device = result.device;
                      final name = device.platformName.isNotEmpty
                          ? device.platformName
                          : 'Unknown Device';

                      return ListTile(
                        leading: const Icon(Icons.bluetooth),
                        title: Text(name),
                        subtitle: Text(device.remoteId.str),
                        trailing: const Icon(Icons.chevron_right),
                        onTap: () => _onDeviceTap(device),
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }
}
