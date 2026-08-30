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

  /// Global monospace text scale — Linux renders small monospace glyphs
  /// hair-thin, so every Text (including graph/knob labels) is enlarged by
  /// this factor on top of the (already bumped) theme sizes.
  static const double _globalTextScale = 1.2;

  const CozySpectraApp({super.key, required this.bleService});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Fruitipi',
      debugShowCheckedModeBanner: false,
      themeMode: ThemeMode.dark,
      darkTheme: Cozy.darkTheme(),
      builder: (context, child) => MediaQuery(
        data: MediaQuery.of(context)
            .copyWith(textScaler: const TextScaler.linear(_globalTextScale)),
        child: child ?? const SizedBox.shrink(),
      ),
      home: CozySpectraDashboard(bleService: bleService),
    );
  }
}
