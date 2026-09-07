import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:app/core/dual_model_wire.dart';
import 'package:app/core/training_repository.dart';

void main() {
  group('DualModelWire.build', () {
    test('canonical defaults match build_dual_model_bin.py layout', () {
      final bin = DualModelWire.build(name: 'Banana Multi v5');
      expect(bin.length, 852);
      expect(DualModelWire.isValid(bin), isTrue);
      expect(DualModelWire.binName(bin), 'Banana Multi v5');

      final bd = ByteData.sublistView(bin);
      // header
      expect([bin[32], bin[33], bin[34], bin[35]], [0x44, 0x4C, 0x41, 0x54]);
      expect(bin[36], 1); // wire_version
      expect(bin[37], 0x1F); // mask
      expect(bin[38], 2); // DUAL
      expect(bin[39], 5); // green_bin_start
      expect(bin[40], 8); // green_bin_end
      // calibration floats (LE)
      expect(bd.getFloat32(48, Endian.little), closeTo(-1.150, 1e-6));
      expect(bd.getFloat32(52, Endian.little), closeTo(100.0, 1e-6));
      expect(bd.getFloat32(56, Endian.little), closeTo(12.0, 1e-6));
      expect(bd.getFloat32(60, Endian.little), closeTo(0.20, 1e-6));
      expect(bd.getFloat32(64, Endian.little), closeTo(0.08, 1e-6));
      expect(bd.getFloat32(68, Endian.little), closeTo(0.5, 1e-6));
      expect(bd.getFloat32(72, Endian.little), closeTo(0.5, 1e-6));
      expect(bd.getFloat32(76, Endian.little), closeTo(10.0, 1e-6));
      expect(bd.getFloat32(80, Endian.little), closeTo(600.0, 1e-6));
      // anchors
      const expectAnchors = [0.10, 0.85, 1.10, 1.30, 0.10];
      for (var c = 0; c < 5; c++) {
        expect(bd.getFloat32(92 + 4 * c, Endian.little),
            closeTo(expectAnchors[c], 1e-6));
      }
      // tail fields
      expect(bd.getUint16(113, Endian.little), 160); // min_peak_adc
      expect(bin[116], 0); // feat_mode
      expect(bd.getFloat32(120, Endian.little), closeTo(0.5, 1e-6));
    });

    test('modality overrides DUAL weights (ACOUSTIC = 1/0)', () {
      final bin = DualModelWire.build(
          name: 'Acoustic', modality: DualModelWire.modalityAcoustic);
      final bd = ByteData.sublistView(bin);
      expect(bd.getFloat32(68, Endian.little), closeTo(1.0, 1e-6));
      expect(bd.getFloat32(72, Endian.little), closeTo(0.0, 1e-6));
      expect(bin[38], 0);
    });

    test('custom controls serialize into the right fields', () {
      final bin = DualModelWire.build(
        name: 'Tuned',
        activeMask: 0x03,
        r0Spec: -1.2300,
        alpha: -12.0,
        wAcoustic: 0.30,
        wOptics: 0.70,
        minPeakAdc: 200,
        featMode: 1,
        stftT1Prior: 0.62,
      );
      final bd = ByteData.sublistView(bin);
      expect(bin[37], 0x03);
      expect(bd.getFloat32(48, Endian.little), closeTo(-1.2300, 1e-6));
      expect(bd.getFloat32(56, Endian.little), closeTo(-12.0, 1e-6));
      expect(bd.getFloat32(68, Endian.little), closeTo(0.30, 1e-6));
      expect(bd.getFloat32(72, Endian.little), closeTo(0.70, 1e-6));
      expect(bd.getUint16(113, Endian.little), 200);
      expect(bin[116], 1);
      expect(bd.getFloat32(120, Endian.little), closeTo(0.62, 1e-6));
    });
  });

  group('TrainingRepository.buildDualWire', () {
    test('returns null without 2 connected classes', () {
      final repo = TrainingRepository.instance;
      final wire = repo.buildDualWire(
          fruitName: 'X', enabledLabels: ['UNRIPE', 'PERFECTLY_RIPE']);
      expect(wire, isNull); // empty repo → no trained classes
    });
  });
}