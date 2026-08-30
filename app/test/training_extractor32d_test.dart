import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:app/core/training_extractor32d.dart';

/// Validates the Dart 32-D extractor against the canonical Python engine
/// (tests/test_fruit_profile_engine.py extract_state_32d_balanced) on the OLD
/// grounded 2-class dataset. Reference values below were produced by that
/// engine (see notes). Max allowed drift is float-precision tolerance.
void main() {
  const dataset =
      '/home/SharedWorkspace/school/project/fruitpi/datacollection/dataset'
      '/Mango/Mango_1785891275';
  const unripeHues = [0.542, 0.019, 0.429, 0.0, 0.0016, 0.0, 0.0, 0.0081];
  const vol = 150.0;

  // Python extract_state_32d_balanced references.
  const refs = <String, List<double>>{
    '01': [
      0.553292, 0.019396, 0.437938, 0.000000, 0.001633, 0.000000, 0.000000,
      0.008269, 0.608639, 0.362198, 0.424298, 0.349024, 0.340709, 0.343339,
      0.205650, 0.247993, 0.304267, 0.308100, 0.158541, 0.175460, 0.211803,
      0.261484, 0.000000, 0.010245, 0.040152, 0.044935, 0.122259, 0.010436,
      0.679389, 0.142824, 0.690832, 0.160392,
    ],
    '03': [
      0.553292, 0.019396, 0.437938, 0.000000, 0.001633, 0.000000, 0.000000,
      0.008269, 0.608639, 0.362198, 0.306450, 0.421046, 0.399004, 0.307758,
      0.264539, 0.217127, 0.266530, 0.277155, 0.203819, 0.171469, 0.255269,
      0.253945, 0.000000, 0.017609, 0.065841, 0.068341, 0.148658, 0.020822,
      0.765324, 0.153326, 0.587747, 0.150965,
    ],
    '06': [
      0.553292, 0.019396, 0.437938, 0.000000, 0.001633, 0.000000, 0.000000,
      0.008269, 0.608639, 0.362198, 0.384462, 0.459610, 0.433816, 0.358766,
      0.272136, 0.244495, 0.248677, 0.283375, 0.104718, 0.022854, 0.000779,
      0.000000, 0.085816, 0.091554, 0.072489, 0.124778, 0.184560, 0.000861,
      0.694121, 0.189407, 0.635814, 0.209755,
    ],
  };

  for (final entry in refs.entries) {
    test('waveform_${entry.key} matches Python extract_state_32d_balanced',
        () {
      final wave = _loadWave('$dataset/waveform_${entry.key}.csv');
      final got = TrainingExtractor32D.extractState32dBalanced(
          wave: wave, visionHues: unripeHues, volumeCm3: vol);
      for (var i = 0; i < 32; i++) {
        final diff = (got[i] - entry.value[i]).abs();
        expect(diff, lessThan(2e-5),
            reason: 'dim $i: dart=${got[i].toStringAsFixed(6)} '
                'py=${entry.value[i].toStringAsFixed(6)}');
      }
    });
  }
}

List<double> _loadWave(String path) {
  final num = RegExp(r'-?\d+(?:\.\d+)?');
  final matches = num.allMatches(File(path).readAsStringSync());
  final out = <double>[];
  for (final m in matches) {
    out.add(double.parse(m.group(0)!));
  }
  return out.take(512).toList();
}
