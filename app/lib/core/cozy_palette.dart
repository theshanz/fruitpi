import 'package:flutter/material.dart';

/// Cozy Warm Dark Palette — single source of truth.
class Cozy {
  Cozy._();

  static const matcha = Color(0xFFA3C9A8);
  static const mintCyan = Color(0xFF9FE0D6);
  static const chamomile = Color(0xFFE8D08D);
  static const heatherPink = Color(0xFFD4A5C1);
  static const duskBlue = Color(0xFF9FB3C8);
  static const linenAlmond = Color(0xFFDDBEA9);

  static const oatmeal = Color(0xFFEDE8DF);
  static const warmGray = Color(0xFFB8B3AA);
  static const dimGray = Color(0xFF8F8A82);
  static const espresso = Color(0xFF131418);
  static const deepBg = Color(0xFF101115);
  static const surfaceCard = Color(0xFF1B1D22);
  static const roseError = Color(0xFFE5989B);

  static const accents = [matcha, chamomile, heatherPink, duskBlue, linenAlmond];

  static const monoFamily = 'monospace';

  static TextTheme textTheme() => const TextTheme(
        displayLarge: TextStyle(
            fontFamily: monoFamily,
            fontWeight: FontWeight.w800,
            fontSize: 34,
            color: oatmeal),
        headlineMedium: TextStyle(
            fontFamily: monoFamily,
            fontWeight: FontWeight.w800,
            fontSize: 26,
            color: oatmeal),
        titleLarge: TextStyle(
            fontFamily: monoFamily,
            fontWeight: FontWeight.w700,
            fontSize: 20,
            color: oatmeal),
        titleMedium: TextStyle(
            fontFamily: monoFamily,
            fontWeight: FontWeight.w700,
            fontSize: 17,
            color: oatmeal),
        bodyLarge: TextStyle(
            fontFamily: monoFamily, fontWeight: FontWeight.w600, fontSize: 16,
            color: oatmeal),
        bodyMedium: TextStyle(
            fontFamily: monoFamily, fontWeight: FontWeight.w600, fontSize: 15,
            color: warmGray),
        labelLarge: TextStyle(
            fontFamily: monoFamily,
            fontWeight: FontWeight.w700,
            fontSize: 15,
            color: oatmeal),
        labelSmall: TextStyle(
            fontFamily: monoFamily, fontWeight: FontWeight.w600, fontSize: 13,
            color: dimGray),
      );

  static ThemeData darkTheme() => ThemeData(
        useMaterial3: true,
        brightness: Brightness.dark,
        colorScheme: const ColorScheme(
          brightness: Brightness.dark,
          primary: matcha,
          onPrimary: Color(0xFF1B2E1E),
          primaryContainer: Color(0xFF2E4032),
          onPrimaryContainer: Color(0xFFD4E9D7),
          secondary: linenAlmond,
          onSecondary: Color(0xFF382E27),
          surface: espresso,
          onSurface: oatmeal,
          surfaceContainer: Color(0xFF1B1D22),
          surfaceContainerHigh: Color(0xFF24272E),
          surfaceContainerHighest: Color(0xFF2D3039),
          outline: Color(0xFF3E414A),
          error: roseError,
          onError: Color(0xFF4A1E20),
        ),
        scaffoldBackgroundColor: deepBg,
        textTheme: textTheme(),
      );
}
