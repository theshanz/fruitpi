import 'dart:ui';

import 'package:flutter/material.dart';

/// Frosted-glass container used across the cozy UI.
class FrostedBox extends StatelessWidget {
  final Widget child;
  final BorderRadius? borderRadius;
  final EdgeInsetsGeometry? padding;
  final Color? backgroundColor;
  final Color? borderColor;

  const FrostedBox({
    super.key,
    required this.child,
    this.borderRadius,
    this.padding,
    this.backgroundColor,
    this.borderColor,
  });

  @override
  Widget build(BuildContext context) {
    final radius = borderRadius ?? BorderRadius.circular(24);
    return ClipRRect(
      borderRadius: radius,
      child: BackdropFilter(
        filter: ImageFilter.blur(sigmaX: 18, sigmaY: 18),
        child: Container(
          padding: padding ?? const EdgeInsets.all(20),
          decoration: BoxDecoration(
            color: backgroundColor ?? Colors.white.withValues(alpha: 0.045),
            borderRadius: radius,
            border: Border.all(
              color: borderColor ?? Colors.white.withValues(alpha: 0.09),
              width: 1.0,
            ),
          ),
          child: child,
        ),
      ),
    );
  }
}

/// Soft radial ambient glow orb.
class AmbientGlow extends StatelessWidget {
  final Color color;
  final double size;

  const AmbientGlow({super.key, required this.color, required this.size});

  @override
  Widget build(BuildContext context) {
    return IgnorePointer(
      child: Container(
        width: size,
        height: size,
        decoration: BoxDecoration(
          shape: BoxShape.circle,
          gradient: RadialGradient(
            colors: [color, Colors.transparent],
            stops: const [0.0, 0.75],
          ),
        ),
      ),
    );
  }
}

/// `// SECTION LABEL` terminal-style heading.
class SectionLabel extends StatelessWidget {
  final String title;
  const SectionLabel({super.key, required this.title});

  @override
  Widget build(BuildContext context) {
    return Text(title,
        style: const TextStyle(
            fontSize: 15,
            fontWeight: FontWeight.bold,
            color: Color(0xFF8F8A82),
            letterSpacing: 1.1));
  }
}

/// Terminal metric pill row.
class MetricPill extends StatelessWidget {
  final String title;
  final String value;
  final Color highlight;

  const MetricPill({
    super.key,
    required this.title,
    required this.value,
    required this.highlight,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
      decoration: BoxDecoration(
        color: Colors.white.withValues(alpha: 0.035),
        borderRadius: BorderRadius.circular(10),
      ),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Text(title,
              style:
                  const TextStyle(fontSize: 15, color: Color(0xFF8F8A82))),
          Flexible(
            child: Text(value,
                textAlign: TextAlign.right,
                style: TextStyle(
                    fontSize: 16, fontWeight: FontWeight.bold, color: highlight)),
          ),
        ],
      ),
    );
  }
}
