import 'package:dynamic_color/dynamic_color.dart';
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'screens/dashboard.dart';
import 'services/ble_service.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  FlutterBluePlus.setLogLevel(LogLevel.warning);
  runApp(const FruitPiApp());
}

class FruitPiApp extends StatefulWidget {
  const FruitPiApp({super.key});

  @override
  State<FruitPiApp> createState() => _FruitPiAppState();
}

class _FruitPiAppState extends State<FruitPiApp> {
  final _bleService = BleService();

  @override
  void dispose() {
    _bleService.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return DynamicColorBuilder(
      builder: (ColorScheme? lightScheme, ColorScheme? darkScheme) {
        final colorScheme = darkScheme ?? ColorScheme.fromSeed(
          seedColor: Colors.teal,
          brightness: Brightness.dark,
        );

        return MaterialApp(
          title: 'FruitPi',
          debugShowCheckedModeBanner: false,
          theme: ThemeData(
            useMaterial3: true,
            brightness: Brightness.dark,
            colorScheme: colorScheme,
            textTheme: Typography().white.merge(ThemeData(brightness: Brightness.dark).textTheme).copyWith(
              displayLarge: ThemeData(brightness: Brightness.dark).textTheme.displayLarge?.copyWith(
                fontWeight: FontWeight.w900,
                letterSpacing: -2,
              ),
              displayMedium: ThemeData(brightness: Brightness.dark).textTheme.displayMedium?.copyWith(
                fontWeight: FontWeight.w900,
                letterSpacing: -1.5,
              ),
              displaySmall: ThemeData(brightness: Brightness.dark).textTheme.displaySmall?.copyWith(
                fontWeight: FontWeight.w800,
                letterSpacing: -1,
              ),
              headlineLarge: ThemeData(brightness: Brightness.dark).textTheme.headlineLarge?.copyWith(
                fontWeight: FontWeight.w800,
              ),
            ),
            cardTheme: CardThemeData(
              elevation: 0,
              shape: RoundedRectangleBorder(
                borderRadius: BorderRadius.circular(24),
              ),
            ),
          ),
          home: DashboardScreen(bleService: _bleService),
        );
      },
    );
  }
}
