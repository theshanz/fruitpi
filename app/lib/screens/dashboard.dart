import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../models/fruit_data.dart';
import '../services/ble_service.dart';
import 'settings.dart';

class DashboardScreen extends StatefulWidget {
  final BleService bleService;
  const DashboardScreen({super.key, required this.bleService});

  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen>
    with TickerProviderStateMixin {
  FruitData _fruitData = FruitData.empty();
  bool _isConnected = false;
  int _selectedFruitIndex = 0;
  int _selectedSizeIndex = 1;

  StreamSubscription? _dataSub;
  StreamSubscription? _connSub;
  late AnimationController _pulseController;
  late AnimationController _resultController;
  late PageController _fruitPageController;

  static const _fruits = [
    _FruitOption('Mango', Icons.eco_rounded, Color(0xFFFFA726)),
    _FruitOption('Apple', Icons.apple_rounded, Color(0xFFEF5350)),
    _FruitOption('Banana', Icons.bakery_dining_rounded, Color(0xFFFFD54F)),
  ];

  static const _sizes = [
    _SizeOption('S', 'Small'),
    _SizeOption('M', 'Med'),
    _SizeOption('L', 'Large'),
  ];

  @override
  void initState() {
    super.initState();
    _fruitPageController = PageController(viewportFraction: 0.38);
    _pulseController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1400),
    )..repeat(reverse: true);

    _resultController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 600),
    );

    _connSub = widget.bleService.onConnectionChange.listen((connected) {
      setState(() => _isConnected = connected);
    });

    _dataSub = widget.bleService.onData.listen((data) {
      setState(() => _fruitData = data);
      _resultController.forward(from: 0);
    });
  }

  @override
  void dispose() {
    _dataSub?.cancel();
    _connSub?.cancel();
    _pulseController.dispose();
    _resultController.dispose();
    _fruitPageController.dispose();
    super.dispose();
  }

  void _openDeviceList() async {
    if (!mounted) return;
    final result = await Navigator.push<BluetoothDevice>(
      context,
      MaterialPageRoute(
          builder: (_) => SettingsScreen(bleService: widget.bleService)),
    );
    if (result != null) {
      await widget.bleService.connect(result);
    }
  }

  bool get _isScanning => _fruitData.status == FruitStatus.scanning;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final cs = theme.colorScheme;

    return Scaffold(
      backgroundColor: cs.surface,
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.only(top: 0),
          children: [
            const SizedBox(height: 12),

            // --- HEADER ---
            Padding(
              padding: const EdgeInsets.fromLTRB(20, 0, 20, 0),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.center,
                children: [
                  // HUGE expressive title
                  Text(
                    'Fruit\nScanner',
                    style: theme.textTheme.displaySmall?.copyWith(
                      height: 1.0,
                    ),
                  ),
                  const Spacer(),
                  _buildConnectPill(theme),
                ],
              ),
            ),
            const SizedBox(height: 28),

            // --- FRUIT CAROUSEL ---
            SizedBox(
              height: 150,
              child: PageView.builder(
                controller: _fruitPageController,
                itemCount: _fruits.length,
                onPageChanged: (i) =>
                    setState(() => _selectedFruitIndex = i),
                itemBuilder: (context, index) {
                  final f = _fruits[index];
                  final selected = index == _selectedFruitIndex;
                  return GestureDetector(
                    onTap: () {
                      if (!selected) {
                        _fruitPageController.animateToPage(index,
                            duration: const Duration(milliseconds: 400),
                            curve: Curves.easeOutCubic);
                      }
                    },
                    child: AnimatedContainer(
                      duration: const Duration(milliseconds: 350),
                      curve: Curves.easeOutCubic,
                      margin: const EdgeInsets.symmetric(horizontal: 5),
                      decoration: BoxDecoration(
                        gradient: selected
                            ? LinearGradient(
                                begin: Alignment.topLeft,
                                end: Alignment.bottomRight,
                                colors: [
                                  f.color.withValues(alpha: 0.3),
                                  f.color.withValues(alpha: 0.05),
                                ],
                              )
                            : null,
                        color: selected ? null : cs.surfaceContainerHighest,
                        borderRadius: BorderRadius.circular(24),
                        border: Border.all(
                          color: selected
                              ? f.color.withValues(alpha: 0.5)
                              : cs.outlineVariant.withValues(alpha: 0.2),
                          width: selected ? 2 : 1,
                        ),
                      ),
                      child: Column(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          Icon(
                            f.icon,
                            size: selected ? 56 : 40,
                            color: selected ? f.color : cs.onSurfaceVariant,
                          ),
                          const SizedBox(height: 10),
                          Text(
                            f.name.toUpperCase(),
                            style: TextStyle(
                              fontSize: 12,
                              fontWeight: FontWeight.w800,
                              letterSpacing: 1.5,
                              color: selected ? f.color : cs.outline,
                            ),
                          ),
                        ],
                      ),
                    ),
                  );
                },
              ),
            ),
            const SizedBox(height: 14),

            // --- SIZE SELECTOR ---
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 20),
              child: Row(
                children: List.generate(3, (i) {
                  final s = _sizes[i];
                  final selected = i == _selectedSizeIndex;
                  return Expanded(
                    child: Padding(
                      padding: EdgeInsets.only(right: i < 2 ? 8 : 0),
                      child: GestureDetector(
                        onTap: () =>
                            setState(() => _selectedSizeIndex = i),
                        child: AnimatedContainer(
                          duration: const Duration(milliseconds: 300),
                          height: 56,
                          decoration: BoxDecoration(
                            color: selected
                                ? cs.primaryContainer
                                : cs.surfaceContainerHighest,
                            borderRadius: BorderRadius.circular(18),
                            border: Border.all(
                              color: selected
                                  ? cs.primary.withValues(alpha: 0.5)
                                  : cs.outlineVariant.withValues(alpha: 0.2),
                              width: selected ? 2 : 1,
                            ),
                          ),
                          child: Center(
                            child: Text(
                              s.letter,
                              style: TextStyle(
                                fontSize: 20,
                                fontWeight: FontWeight.w900,
                                color: selected ? cs.primary : cs.outline,
                              ),
                            ),
                          ),
                        ),
                      ),
                    ),
                  );
                }),
              ),
            ),
            const SizedBox(height: 20),

            // --- STATUS / RESULT ---
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 20),
              child: _isScanning
                  ? _buildScanningCard(theme)
                  : !_fruitData.isEmpty
                      ? _buildResultCard(theme)
                      : _buildIdleCard(theme),
            ),
            const SizedBox(height: 32),
          ],
        ),
      ),
    );
  }

  Widget _buildConnectPill(ThemeData theme) {
    final cs = theme.colorScheme;
    return ActionChip(
      onPressed: _openDeviceList,
      avatar: Icon(
        _isConnected
            ? Icons.bluetooth_connected_rounded
            : Icons.bluetooth_disabled_rounded,
        size: 18,
        color: _isConnected ? cs.onPrimaryContainer : cs.onErrorContainer,
      ),
      label: Text(
        _isConnected ? 'Connected' : 'Connect',
        style: theme.textTheme.labelMedium?.copyWith(
          color: _isConnected ? cs.onPrimaryContainer : cs.onErrorContainer,
        ),
      ),
      color: WidgetStateProperty.resolveWith<Color?>(
        (Set<WidgetState> states) {
          if (_isConnected) {
            return cs.primaryContainer;
          }
          return cs.errorContainer;
        },
      ),
      padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 0),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(100),
        side: BorderSide(
          color: _isConnected
              ? cs.primary.withOpacity(0.5)
              : cs.error.withOpacity(0.5),
          width: 1,
        ),
      ),
    );
  }

  Widget _buildIdleCard(ThemeData theme) {
    final cs = theme.colorScheme;
    return Card(
      elevation: 2,
      color: cs.surface,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(24),
        side: BorderSide(
          color: cs.outlineVariant.withValues(alpha: 0.2),
          width: 1,
        ),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 48),
        child: Column(
          children: [
            Icon(
              _isConnected
                  ? Icons.sensors_rounded
                  : Icons.bluetooth_searching_rounded,
              size: 52,
              color: cs.outline,
            ),
            const SizedBox(height: 16),
            Text(
              _isConnected ? 'Ready to scan' : 'Connect to ESP32',
              style:
                  theme.textTheme.titleMedium?.copyWith(color: cs.outline),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildScanningCard(ThemeData theme) {
    final cs = theme.colorScheme;
    final fruit = _fruits[_selectedFruitIndex];

    return Card(
      elevation: 2,
      color: cs.surface,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(24),
        side: BorderSide(
          color: cs.outlineVariant.withValues(alpha: 0.2),
          width: 1,
        ),
      ),
      child: AnimatedBuilder(
        animation: _pulseController,
        builder: (context, _) {
          final t = _pulseController.value;
          return Container(
            padding: const EdgeInsets.symmetric(vertical: 40),
            decoration: BoxDecoration(
              gradient: LinearGradient(
                begin: Alignment.topLeft,
                end: Alignment.bottomRight,
                colors: [
                  cs.primaryContainer.withValues(alpha: 0.35 + t * 0.15),
                  cs.primaryContainer.withValues(alpha: 0.1),
                ],
              ),
              borderRadius: BorderRadius.circular(24),
            ),
            child: Column(
              children: [
                SizedBox(
                  width: 80,
                  height: 80,
                  child: Stack(
                    alignment: Alignment.center,
                    children: [
                      CircularProgressIndicator(
                        strokeWidth: 4,
                        color: cs.primary.withValues(alpha: 0.2),
                      ),
                      CircularProgressIndicator(
                        strokeWidth: 4,
                        strokeCap: StrokeCap.round,
                        value: null,
                        color: cs.primary,
                      ),
                      Icon(fruit.icon,
                          size: 34,
                          color: cs.onPrimaryContainer),
                    ],
                  ),
                ),
                const SizedBox(height: 20),
                Text(
                  'Scanning',
                  style: theme.textTheme.displaySmall?.copyWith(
                    color: cs.primary,
                  ),
                ),
                const SizedBox(height: 4),
                Text(
                  'analyzing ${fruit.name.toLowerCase()}...',
                  style: theme.textTheme.bodyLarge?.copyWith(
                    color: cs.outline,
                  ),
                ),
              ],
            ),
          );
        },
      ),
    );
  }

  Widget _buildResultCard(ThemeData theme) {
    final cs = theme.colorScheme;
    final score = _fruitData.ripenessScore;
    final status = _fruitData.status;
    final fruit = _fruits[_selectedFruitIndex];

    Color accent;
    switch (status) {
      case FruitStatus.unripe:
        accent = Colors.orange;
      case FruitStatus.ripe:
        accent = Colors.green;
      case FruitStatus.overripe:
        accent = Colors.amber;
      case FruitStatus.rotten:
        accent = Colors.red;
      case FruitStatus.scanning:
        accent = cs.primary;
      case FruitStatus.unknown:
        accent = cs.outline;
    }

    return Card(
      elevation: 4,
      color: cs.surface,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(24),
        side: BorderSide(
          color: cs.outlineVariant.withValues(alpha: 0.2),
          width: 1,
        ),
      ),
      child: ScaleTransition(
        scale: CurvedAnimation(
          parent: _resultController,
          curve: Curves.easeOutBack,
        ),
        child: Container(
          padding: const EdgeInsets.all(24),
          decoration: BoxDecoration(
            gradient: LinearGradient(
              begin: Alignment.topLeft,
              end: Alignment.bottomRight,
              colors: [
                accent.withValues(alpha: 0.18),
                cs.surfaceContainerHighest,
              ],
            ),
            borderRadius: BorderRadius.circular(24),
          ),
          child: Column(
            children: [
              // Big emoji
              Icon(status.icon, size: 64, color: accent),
              const SizedBox(height: 8),

              // Status pill
              Container(
                padding:
                    const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
                decoration: BoxDecoration(
                  color: accent.withValues(alpha: 0.2),
                  borderRadius: BorderRadius.circular(100),
                ),
                child: Text(
                  status.label.toUpperCase(),
                  style: theme.textTheme.labelLarge?.copyWith(
                    color: accent,
                    letterSpacing: 1.5,
                  ),
                ),
              ),
              const SizedBox(height: 20),

              // BIG expressive score
              Row(
                crossAxisAlignment: CrossAxisAlignment.end,
                children: [
                  Text(
                    '${score.toInt()}',
                    style: theme.textTheme.displayMedium?.copyWith(
                      color: accent,
                      height: 1,
                    ),
                  ),
                  Padding(
                    padding: const EdgeInsets.only(bottom: 8, left: 2),
                    child: Text(
                      '%',
                      style: theme.textTheme.headlineMedium?.copyWith(
                        color: accent.withValues(alpha: 0.5),
                        fontWeight: FontWeight.w800,
                      ),
                    ),
                  ),
                  const Spacer(),
                  Text(
                    'ripeness',
                    style: theme.textTheme.bodyMedium?.copyWith(
                      color: cs.outline,
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 10),

              // Bar
              ClipRRect(
                borderRadius: BorderRadius.circular(10),
                child: TweenAnimationBuilder<double>(
                  tween: Tween(begin: 0, end: score / 100),
                  duration: const Duration(milliseconds: 800),
                  curve: Curves.easeOutCubic,
                  builder: (_, value, __) => LinearProgressIndicator(
                    value: value,
                    minHeight: 12,
                    color: accent,
                    backgroundColor: accent.withValues(alpha: 0.1),
                  ),
                ),
              ),
              const SizedBox(height: 20),
              Divider(color: cs.outlineVariant.withValues(alpha: 0.25)),
              const SizedBox(height: 14),

              // Fruit + Size
              Row(
                children: [
                  _infoChip(theme, Icons.local_florist_rounded, 'Fruit',
                      _fruitData.fruitType.isNotEmpty
                          ? _fruitData.fruitType
                          : fruit.name),
                  const Spacer(),
                  _infoChip(theme, Icons.straighten_rounded, 'Size',
                      _fruitData.size.isNotEmpty
                          ? _fruitData.size
                          : _sizes[_selectedSizeIndex].label),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _infoChip(
      ThemeData theme, IconData icon, String label, String value) {
    final cs = theme.colorScheme;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            Icon(icon, size: 14, color: cs.onSurfaceVariant),
            const SizedBox(width: 4),
            Text(
              label.toUpperCase(),
              style: theme.textTheme.labelMedium?.copyWith(
                color: cs.outline,
                letterSpacing: 1,
                fontSize: 10,
              ),
            ),
          ],
        ),
        const SizedBox(height: 4),
        Text(value, style: theme.textTheme.titleMedium),
      ],
    );
  }
}

class _FruitOption {
  final String name;
  final IconData icon;
  final Color color;
  const _FruitOption(this.name, this.icon, this.color);
}

class _SizeOption {
  final String letter;
  final String label;
  const _SizeOption(this.letter, this.label);
}
