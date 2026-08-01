import 'dart:async';

import 'dart:io' show Platform;

import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import '../models/fruit_data.dart';

/// Handles all BLE operations: scanning, connecting, reading.
class BleService {
  // TODO: Replace with your ESP32's actual UUIDs
  static const String serviceUuid = '00001234-0000-1000-8000-00805f9b34fb';
  static const String characteristicUuid =
      '00005678-0000-1000-8000-00805f9b34fb';

  BluetoothDevice? _device;

  final _dataController = StreamController<FruitData>.broadcast();
  Stream<FruitData> get onData => _dataController.stream;

  final _connectionController = StreamController<bool>.broadcast();
  Stream<bool> get onConnectionChange => _connectionController.stream;

  bool get isConnected => _device?.isConnected ?? false;
  BluetoothDevice? get device => _device;

  /// Request BLE + location permissions (required on Android).
  /// On Linux, BlueZ handles permissions via D-Bus — skip this.
  Future<bool> requestPermissions() async {
    if (Platform.isLinux) return true;

    final statuses = await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.location,
    ].request();

    return statuses.values.every((s) => s.isGranted);
  }

  /// Scan for nearby ESP32 devices.
  Stream<List<ScanResult>> scan() {
    print('[BLE] Starting scan...');
    FlutterBluePlus.startScan(timeout: const Duration(seconds: 10));
    FlutterBluePlus.scanResults.listen((results) {
      print('[BLE] Scan results: ${results.length} devices');
      for (final r in results) {
        print('[BLE]   - ${r.device.platformName} (${r.device.remoteId.str}) rssi=${r.rssi}');
      }
    });
    return FlutterBluePlus.scanResults;
  }

  void stopScan() => FlutterBluePlus.stopScan();

  /// Connect to a device and subscribe to fruit data.
  Future<bool> connect(BluetoothDevice device) async {
    try {
      _device = device;
      print('[BLE] Connecting to ${device.platformName}...');
      await device.connect(timeout: const Duration(seconds: 10));
      print('[BLE] Connected! Discovering services...');
      _connectionController.add(true);

      final services = await device.discoverServices();
      print('[BLE] Found ${services.length} services');

      for (final service in services) {
        print('[BLE]   Service: ${service.uuid.str}');
        for (final char in service.characteristics) {
          print('[BLE]     Char: ${char.uuid.str} props=${char.properties}');
          // Match both short (5678) and full (00005678-...) UUID forms
          // BlueZ on Linux returns short UUIDs like "5678" without leading zeros
          final charUuid = char.uuid.str.toLowerCase();
          final targetUuid = characteristicUuid.toLowerCase();
          // Extract short form: last 8 hex chars before the rest, e.g. "5678"
          final targetShort8 = targetUuid.substring(4, 8); // "5678"
          final targetShortFull = targetUuid.substring(0, 8); // "00005678"
          if (charUuid == targetUuid ||
              charUuid == targetShort8 ||
              charUuid == targetShortFull) {
            print('[BLE]     >>> MATCH! Subscribing to notifications...');
            await char.setNotifyValue(true);
            print('[BLE]     >>> Notifications enabled. Listening...');
            char.lastValueStream.listen((value) {
              print('[BLE]     >>> Got data: $value');
              if (value.isNotEmpty) {
                _dataController.add(FruitData.fromBytes(value));
              }
            });
            return true;
          }
        }
      }

      print('[BLE] Characteristic $characteristicUuid NOT FOUND');
      return false; // characteristic not found
    } catch (e) {
      print('[BLE] Connection error: $e');
      _connectionController.add(false);
      return false;
    }
  }

  /// Disconnect from current device.
  Future<void> disconnect() async {
    await _device?.disconnect();
    _device = null;
    _connectionController.add(false);
  }

  void dispose() {
    _dataController.close();
    _connectionController.close();
  }
}
