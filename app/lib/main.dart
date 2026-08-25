import 'package:flutter/material.dart';

import 'core/cozy_palette.dart';
import 'screens/dashboard_screen.dart';
import 'services/ble_service.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(CozySpectraApp(bleService: BleService()));
}

class CozySpectraApp extends StatelessWidget {
  final BleService bleService;
  const CozySpectraApp({super.key, required this.bleService});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Fruitipi',
      debugShowCheckedModeBanner: false,
      themeMode: ThemeMode.dark,
      darkTheme: Cozy.darkTheme(),
      home: CozySpectraDashboard(bleService: bleService),
    );
  }
}
