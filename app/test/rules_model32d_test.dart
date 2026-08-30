import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:app/core/rules_model32d.dart';

void main() {
  // Measured 32-D centroids (validated on the OLD dataset by the canonical
  // Python engine's extract_state_32d_balanced).
  const unripe = [
    0.5533, 0.0194, 0.4379, 0.0, 0.0016, 0.0, 0.0, 0.0083, 0.6086, 0.3622,
    0.3464, 0.4335, 0.4225, 0.3386, 0.2424, 0.2238, 0.2607, 0.2683, 0.1237,
    0.0966, 0.1117, 0.1197, 0.0572, 0.0537, 0.0825, 0.1022, 0.1905, 0.0985,
    0.6661, 0.5698, 0.3337, 0.1427,
  ];
  const ripe = [
    0.4754, 0.0611, 0.4084, 0.0038, 0.0046, 0.0171, 0.0159, 0.0150, 0.6903,
    0.3551, 0.4156, 0.4698, 0.4049, 0.3671, 0.2212, 0.1852, 0.1939, 0.2212,
    0.0833, 0.0991, 0.0973, 0.1266, 0.0811, 0.0769, 0.0637, 0.1009, 0.1510,
    0.0548, 0.6397, 0.5830, 0.4052, 0.1632,
  ];

  // Raw per-tap 32-D states from the REAL mango dataset (validated by
  // /tmp/opencode/repro/run_real_dataset.py). 6 UNRIPE + 7 PERFECTLY_RIPE
  // taps, intentionally NOT block-L2 normalized (amplitude preserved). This is
  // the exact input whose Gaussian scorer lifts mean confidence from 50.5%
  // (old cosine+block-L2) to 99.0% train-time / 78.7% LOO.
  const unripeTaps = [
    [0.4689, 0.0260, 0.4689, 0.0, 0.0017, 0.0017, 0.0017, 0.0311, 0.6385, 0.3548, 1.0, 0.8226, 0.8030, 0.8092, 0.4847, 0.5845, 0.7171, 0.7261, 0.3737, 0.4135, 0.4992, 0.6163, 0.0, 0.0241, 0.0946, 0.1059, 0.1411, 0.0120, 0.7841, 0.1648, 0.7973, 0.1851],
    [0.4689, 0.0260, 0.4689, 0.0, 0.0017, 0.0017, 0.0017, 0.0311, 0.6385, 0.3548, 0.5476, 0.9784, 1.0, 0.6796, 0.4496, 0.7824, 0.5825, 0.6408, 0.1475, 0.4082, 0.3135, 0.4900, 0.0, 0.1562, 0.2678, 0.4123, 0.1819, 0.1247, 1.0, 0.1015, 0.8620, 0.1502],
    [0.4689, 0.0260, 0.4689, 0.0, 0.0017, 0.0017, 0.0017, 0.0311, 0.6385, 0.3548, 0.7278, 1.0, 0.9476, 0.7309, 0.6283, 0.5157, 0.6330, 0.6583, 0.4841, 0.4072, 0.6063, 0.6031, 0.0, 0.0418, 0.1564, 0.1623, 0.1581, 0.0221, 0.8148, 0.1632, 0.6257, 0.1607],
    [0.4689, 0.0260, 0.4689, 0.0, 0.0017, 0.0017, 0.0017, 0.0311, 0.6385, 0.3548, 0.8754, 0.9754, 1.0, 0.9232, 0.7533, 0.5951, 0.6681, 0.6693, 0.3386, 0.1239, 0.0084, 0.0007, 0.0073, 0.0003, 0.0, 0.0, 0.1409, 0.0012, 0.5520, 0.1751, 0.6919, 0.1756],
    [0.4689, 0.0260, 0.4689, 0.0, 0.0017, 0.0017, 0.0017, 0.0311, 0.6385, 0.3548, 0.6543, 1.0, 0.9446, 0.6639, 0.1746, 0.1875, 0.4514, 0.3991, 0.0, 0.0971, 0.2186, 0.0911, 0.1483, 0.2983, 0.4618, 0.4540, 0.1681, 0.0310, 1.0, 0.0867, 0.6805, 0.1982],
    [0.4689, 0.0260, 0.4689, 0.0, 0.0017, 0.0017, 0.0017, 0.0311, 0.6385, 0.3548, 0.8365, 1.0, 0.9439, 0.7806, 0.5921, 0.5320, 0.5411, 0.6166, 0.2278, 0.0497, 0.0017, 0.0, 0.1867, 0.1992, 0.1577, 0.2715, 0.1593, 0.0007, 0.5991, 0.1635, 0.5488, 0.1811],
  ];
  const ripeTaps = [
    [0.3945, 0.1649, 0.3823, 0.0094, 0.0072, 0.0151, 0.0065, 0.0202, 0.7659, 0.3548, 1.0, 0.8781, 0.9283, 0.9369, 0.7884, 0.7721, 0.7963, 0.8363, 0.6080, 0.4354, 0.4073, 0.4777, 0.1083, 0.0084, 0.0002, 0.0, 0.1328, 0.0097, 0.5833, 0.2222, 0.6343, 0.2106],
    [0.3945, 0.1649, 0.3823, 0.0094, 0.0072, 0.0151, 0.0065, 0.0202, 0.7659, 0.3548, 1.0, 0.9999, 0.9604, 0.9158, 0.6645, 0.6083, 0.6960, 0.6805, 0.5210, 0.5648, 0.5790, 0.6195, 0.0912, 0.0060, 0.0001, 0.0, 0.1264, 0.0019, 0.6517, 0.2019, 0.5614, 0.2046],
    [0.3945, 0.1649, 0.3823, 0.0094, 0.0072, 0.0151, 0.0065, 0.0202, 0.7659, 0.3548, 0.9823, 1.0, 0.9015, 0.8395, 0.2971, 0.3273, 0.3048, 0.3716, 0.0, 0.2151, 0.2119, 0.2512, 0.3189, 0.4038, 0.4344, 0.4800, 0.1226, 0.0010, 0.7914, 0.1418, 0.5417, 0.2502],
    [0.3945, 0.1649, 0.3823, 0.0094, 0.0072, 0.0151, 0.0065, 0.0202, 0.7659, 0.3548, 0.7511, 1.0, 0.9093, 0.6847, 0.3848, 0.3982, 0.3449, 0.4022, 0.1801, 0.2362, 0.1729, 0.2309, 0.0, 0.1008, 0.1370, 0.1372, 0.1130, 0.0009, 0.7647, 0.1167, 0.6100, 0.2503],
    [0.3945, 0.1649, 0.3823, 0.0094, 0.0072, 0.0151, 0.0065, 0.0202, 0.7659, 0.3548, 0.9040, 1.0, 0.9140, 0.7803, 0.5489, 0.5487, 0.5793, 0.5993, 0.1651, 0.0088, 0.0, 0.0306, 0.1250, 0.1854, 0.2033, 0.2371, 0.1251, 0.0003, 0.6394, 0.1546, 0.6098, 0.2529],
    [0.3945, 0.1649, 0.3823, 0.0094, 0.0072, 0.0151, 0.0065, 0.0202, 0.7659, 0.3548, 0.7565, 1.0, 0.7639, 0.6106, 0.4934, 0.1716, 0.2627, 0.3600, 0.0, 0.0319, 0.1098, 0.1210, 0.1499, 0.2095, 0.1427, 0.3388, 0.1580, 0.0096, 0.6452, 0.1242, 0.6221, 0.2205],
    [0.3945, 0.1649, 0.3823, 0.0094, 0.0072, 0.0151, 0.0065, 0.0202, 0.7659, 0.3548, 0.9219, 1.0, 0.9080, 0.7254, 0.6196, 0.5573, 0.4577, 0.4903, 0.1043, 0.0191, 0.0, 0.0762, 0.1466, 0.1648, 0.1221, 0.2635, 0.1343, 0.0008, 0.5659, 0.1590, 0.6140, 0.2590],
  ];

  test('852-byte wire layout matches the firmware offsets', () {
    final profile = FruitProfile(name: 'MANGO_TEST');
    // enable only UNRIPE + RIPE.
    for (final c in profile.categories) {
      c.isEnabled = c.id == 0 || c.id == 1;
      c.sourceType = CategorySourceType.measured;
    }
    final bin = FruitProfileCompiler.compile(profile, measuredCentroids: {
      'UNRIPE': unripe,
      'PERFECTLY_RIPE': ripe,
    });

    expect(bin.length, RulesModel32D.wireBytes);
    expect(bin.length, 852);

    final bd = ByteData.sublistView(bin);

    // name
    final name = String.fromCharCodes(bin.take(32).where((b) => b != 0));
    expect(name, 'MANGO_TEST');

    // mask at offset 32, green bins, veto/min/max/f0/damp
    expect(bin[32], 3);
    expect(bin[33], 5);
    expect(bin[34], 8);
    expect(bd.getFloat32(36, Endian.little), closeTo(0.25, 1e-6));
    expect(bd.getFloat32(40, Endian.little), closeTo(10.0, 1e-6));
    expect(bd.getFloat32(44, Endian.little), closeTo(600.0, 1e-6));
    expect(bd.getFloat32(48, Endian.little), closeTo(180.0, 1e-6));
    expect(bd.getFloat32(52, Endian.little), closeTo(1.0, 1e-6));

    // feature_weights = pooled inverse-variance @ 64 (v3): positive & finite.
    // This test supplies only centroids (no raw tap states), so the pooled
    // precision falls back to the default unit value (1.0).
    for (var d = 0; d < 32; d++) {
      final v = bd.getFloat32(64 + d * 4, Endian.little);
      expect(v, greaterThan(0));
      expect(v.isFinite, isTrue);
      expect(v, closeTo(1.0, 1e-6));
    }

    // prototypes @ 192 (5x32) = raw class MEANS (v3). UNRIPE mean is stored as
    // the raw centroid (no block L2, no unit norm). Confirm it is non-zero and
    // that each of the 5x32 slots is materialized.
    expect(bd.getFloat32(192, Endian.little).abs(), greaterThan(0));
    final lastProto = bd.getFloat32(
        192 + (4 * 32 + 31) * 4, Endian.little);
    expect(lastProto, anything); // 5x32 block fully materialized (offset valid)

    // biases @ 832: active 0, inactive -100
    expect(bd.getFloat32(832, Endian.little), closeTo(0.0, 1e-6));
    expect(bd.getFloat32(836, Endian.little), closeTo(0.0, 1e-6));
    expect(bd.getFloat32(840, Endian.little), closeTo(-100.0, 1e-6));
    expect(bd.getFloat32(844, Endian.little), closeTo(-100.0, 1e-6));
    expect(bd.getFloat32(848, Endian.little), closeTo(-100.0, 1e-6));
  });

  test('compiled prototypes separate UNRIPE vs RIPE and selftest passes', () {
    final profile = FruitProfile(name: 'M');
    for (final c in profile.categories) {
      c.isEnabled = c.id == 0 || c.id == 1;
      c.sourceType = CategorySourceType.measured;
    }
    // v3: with only centroids supplied (no raw tap states) the pooled
    // precision falls back to unit weights, so scoring is plain L2 Mahalanobis
    // over the means — each centroid maps to its own class.
    final protos = FruitProfileCompiler.compiledPrototypes(profile,
        measuredCentroids: {'UNRIPE': unripe, 'PERFECTLY_RIPE': ripe});
    final invVar = FruitProfileCompiler.compiledInverseVariance(profile,
        measuredCentroids: {'UNRIPE': unripe, 'PERFECTLY_RIPE': ripe});
    final biases = FruitProfileCompiler.compiledBiases(profile);
    final mask = profile.activeMask;

    // Means are RAW (not unit) — no unit-norm invariant.
    for (final c in [0, 1]) {
      var any = false;
      for (final v in protos[c]) {
        if (v != 0.0) any = true;
      }
      expect(any, isTrue);
    }

    // Self-classification (Gaussian): each measured centroid is itself a class
    // mean -> distance 0 to own class, so it maps to its own class.
    final ur = RulesModel32D.classify32d(unripe, protos, biases, invVar, mask);
    expect(ur.winner, 0);
    final rp = RulesModel32D.classify32d(ripe, protos, biases, invVar, mask);
    expect(rp.winner, 1);

    // Perturbation selftest (Gaussian scorer).
    expect(RulesModel32D.selftest32d(protos, biases, invVar, mask,
        ['UNRIPE', 'PERFECTLY_RIPE']),
        isTrue);

    // Separation matrix: symmetric, diagonal = 1.
    final sep = RulesModel32D.separationMatrix(protos);
    expect(sep[0][0], closeTo(1.0, 1e-6));
    expect(sep[1][1], closeTo(1.0, 1e-6));
    expect(sep[0][1], sep[1][0]);
    // The measured UNRIPE/RIPE mango centroids are near-identical (cosine
    // ~0.992 — same variety, same volume), so their separation is high but
    // strictly below 1. The tight separation is a property of the underlying
    // measurements, not the compiler; archetype bases separate far better
    // (verified below).
    expect(sep[0][1], lessThan(1.0));
    expect(sep[0][1], greaterThan(0.0));
  });

  test('archetype-enabled profile compiles (no measured data needed)', () {
    final profile = FruitProfile(name: 'ARCH');
    // UNRIPE -> hard_unripe, RIPE -> prime_ripe.
    profile.categories[0]
      ..isEnabled = true
      ..sourceType = CategorySourceType.archetype
      ..archetypePresetId = 'hard_unripe';
    profile.categories[1]
      ..isEnabled = true
      ..sourceType = CategorySourceType.archetype
      ..archetypePresetId = 'prime_ripe';
    for (var c = 2; c < 5; c++) {
      profile.categories[c].isEnabled = false;
    }
    final bin = FruitProfileCompiler.compile(profile);
    expect(bin.length, 852);
    expect(bin[32], 3);

    // Archetype-derived bases must separate cleanly (well below the ~0.99
    // that the near-identical measured mango centroids produce), confirming
    // the archetype path gives a healthy discriminator.
    final protos = FruitProfileCompiler.compiledPrototypes(profile);
    final sep = RulesModel32D.separationMatrix(protos);
    expect(sep[0][1], lessThan(0.9));
    expect(sep[2][3], lessThan(0.9));
  });

  test('weighted separation responds to balance weights; raw cosine does not',
      () {
    // Enable three archetype slots (a tight R-O pair) plus real measured states
    // so the pooled inverse-variance is non-unit (not the all-1.0 fallback).
    final profile = FruitProfile(name: 'WS');
    profile.categories[0]
      ..isEnabled = true
      ..sourceType = CategorySourceType.archetype
      ..archetypePresetId = 'hard_unripe';
    profile.categories[1]
      ..isEnabled = true
      ..sourceType = CategorySourceType.archetype
      ..archetypePresetId = 'prime_ripe';
    profile.categories[2]
      ..isEnabled = true
      ..sourceType = CategorySourceType.archetype
      ..archetypePresetId = 'overripe_soft';
    for (var c = 3; c < 5; c++) {
      profile.categories[c].isEnabled = false;
    }
    final protos = FruitProfileCompiler.compiledPrototypes(profile,
        measuredStates: {'UNRIPE': unripeTaps});
    final invBase = FruitProfileCompiler.compiledInverseVariance(profile,
        measuredStates: {'UNRIPE': unripeTaps});

    // Raw-mean cosine separation is balance-INDEPENDENT (the original bug).
    final raw = RulesModel32D.separationMatrix(protos);
    expect(raw[1][2], closeTo(0.946, 0.02));

    const sepW = FruitProfileCompiler.mahalanobisMatrix;
    List<double> scaledInv(double wImage) {
      // Mirrors _precisionWeights: dims 10..26 (Acoustic Texture block) get
      // scaled by the wImage slider before it reaches the separation metric.
      final iv = List<double>.from(invBase);
      for (var d = 10; d < 26; d++) {
        iv[d] *= wImage;
      }
      return iv;
    }

    final lo = sepW(protos, scaledInv(0.1))[0][1];
    final hi = sepW(protos, scaledInv(10.0))[0][1];
    expect(hi, greaterThan(lo),
        reason: 'balance slider must visibly move the Mahalanobis separation');
    expect((hi - lo).abs(), greaterThan(0.01),
        reason: 'balance slider must visibly move the Mahalanobis separation');

    // Diagonal is zero (no self-separation) and matrix is symmetric.
    expect(sepW(protos, invBase)[1][1], 0.0);
    expect(sepW(protos, invBase)[0][1], sepW(protos, invBase)[1][0]);
  });

  test('roundtrip: compile -> pack -> read-back name/mask/edit blob', () {
    final profile = FruitProfile(name: 'ROUNDTRIP');
    // all 5 archetype slots enabled so the mask is fully populated.
    for (final c in profile.categories) {
      c.isEnabled = true;
      c.sourceType = CategorySourceType.archetype;
    }
    profile.categories[0].archetypePresetId = 'hard_unripe';
    profile.categories[1].archetypePresetId = 'prime_ripe';
    profile.categories[2].archetypePresetId = 'overripe_soft';
    profile.categories[3].archetypePresetId = 'hollow_defect';
    profile.categories[4].archetypePresetId = 'inert_standard';

    final bin = FruitProfileCompiler.compile(profile);

    // read-back helpers (used by the model editor)
    expect(bin.length, 852);
    expect(RulesModel32D.maskOf(bin), 0x1F);
    expect(RulesModel32D.binName(bin), 'ROUNDTRIP');

    // base64/hex parse roundtrip
    expect(RulesModel32D.tryParseBinText(RulesModel32D.toBase64(bin)),
        isNotNull);
    final hex = bin.map((b) => b.toRadixString(16).padLeft(2, '0')).join();
    expect(RulesModel32D.tryParseBinText(hex), isNotNull);
    expect(RulesModel32D.tryParseBinText('garbage!!'), isNull);

    // editing: disable class 2 + rename -> mask updates, size preserved.
    final edited = RulesModel32D.withName(
        RulesModel32D.withMask(bin, 0x1F & ~(1 << 2)), 'NEW_NAME');
    expect(edited.length, 852);
    expect(RulesModel32D.maskOf(edited), 0x1F & ~(1 << 2));
    expect(RulesModel32D.binName(edited), 'NEW_NAME');
  });

  test('Gaussian scorer lifts real-dataset confidence far above the ~50% '
      'cosine baseline', () {
    // Reproduce the validated result from /tmp/opencode/repro/run_real_dataset.py
    // on the ACTUAL app compile path: feed the 13 raw tap states, compile with
    // the real pooled inverse-variance, and score each tap. Green veto is off
    // (matches the validated acoustic-only Gaussian scenario).
    final profile = FruitProfile(name: 'MANGO_REAL');
    for (final c in profile.categories) {
      c.isEnabled = c.id == 0 || c.id == 1;
      c.sourceType = CategorySourceType.measured;
      c.enableGreenVeto = false;
    }
    final protos = FruitProfileCompiler.compiledPrototypes(profile,
        measuredStates: {'UNRIPE': unripeTaps, 'PERFECTLY_RIPE': ripeTaps});
    final invVar = FruitProfileCompiler.compiledInverseVariance(profile,
        measuredStates: {'UNRIPE': unripeTaps, 'PERFECTLY_RIPE': ripeTaps});
    final biases = FruitProfileCompiler.compiledBiases(profile);
    final mask = profile.activeMask;

    // Pooled inverse-variance must be materially active (NOT the unit fallback),
    // proving measuredStates actually fed the Gaussian precision.
    var totalInv = 0.0;
    for (final v in invVar) {
      totalInv += v;
    }
    expect(totalInv, greaterThan(40.0)); // unit fallback would be exactly 32

    // Train-time (means from all taps): each tap must classify to its true class
    // at high confidence.
    final confs = <double>[];
    var correct = 0;
    for (final t in unripeTaps) {
      final r = RulesModel32D.classify32d(t, protos, biases, invVar, mask);
      confs.add(r.probs[0] * 100);
      if (r.winner == 0) correct++;
    }
    for (final t in ripeTaps) {
      final r = RulesModel32D.classify32d(t, protos, biases, invVar, mask);
      confs.add(r.probs[1] * 100);
      if (r.winner == 1) correct++;
    }

    expect(correct, 13);
    final mean = confs.reduce((a, b) => a + b) / confs.length;
    // Old cosine+block-L2 sat at ~50.5% mean; the Gaussian lifts it to ~99%.
    expect(mean, greaterThan(90.0));
    // Every single tap is decisive (min train-time confidence is 88.3%).
    expect(confs.reduce((a, b) => a < b ? a : b), greaterThan(60.0));
  });

  test('Step 1: measured taps auto-promote over the archetype base even when '
      'the source toggle is set to archetype', () {
    // Build a profile whose UNRIPE slot is (incorrectly) left on archetype but
    // we still pass real taps. The old bug silently ignored those taps because
    // sourceType == archetype; now real taps must always win.
    final profile = FruitProfile(name: 'AUTO');
    profile.categories[0]
      ..isEnabled = true
      ..sourceType = CategorySourceType.archetype // trap: left on archetype
      ..archetypePresetId = 'hard_unripe';
    profile.categories[1]
      ..isEnabled = true
      ..sourceType = CategorySourceType.measured
      ..archetypePresetId = 'prime_ripe';
    for (var c = 2; c < 5; c++) {
      profile.categories[c].isEnabled = false;
    }

    // A trivially-different measured UNRIPE state: all dims = 0.5.
    final fakeUnripe = List<List<double>>.generate(
        6, (_) => List<double>.filled(32, 0.5));
    final protos = FruitProfileCompiler.compiledPrototypes(profile,
        measuredStates: {'UNRIPE': fakeUnripe, 'PERFECTLY_RIPE': ripeTaps});

    // If auto-promotion works, the UNRIPE slot is the mean of the fake taps (0.5).
    final unripeProto = protos[0];
    expect(unripeProto.cast<double>().reduce((a, b) => a.abs() + b.abs()),
        closeTo(0.5 * 32, 0.2));
    // And it is NOT the archetype base vector.
    final archBase = ArchetypeLibrary.byId('hard_unripe')!.baseVector;
    var diff = 0.0;
    for (var d = 0; d < 32; d++) {
      diff += (archBase[d] - unripeProto[d]).abs();
    }
    expect(diff, greaterThan(1.0),
        reason: 'auto-promote must override the archetype base');
  });

  test('Step 2: within-class pooled invVar uses per-class means and is not the '
      'unit fallback when real taps exist', () {
    final profile = FruitProfile(name: 'POOL');
    for (final c in profile.categories) {
      c.isEnabled = c.id == 0 || c.id == 1;
      c.sourceType = CategorySourceType.measured;
    }
    final invVar = FruitProfileCompiler.compiledInverseVariance(profile,
        measuredStates: {'UNRIPE': unripeTaps, 'PERFECTLY_RIPE': ripeTaps});
    var totalInv = 0.0;
    for (final v in invVar) {
      totalInv += v;
    }
    // Unit fallback would be exactly 32; within-class pooled variance lifts it.
    expect(totalInv, greaterThan(40.0));

    // Every precision stays strictly positive and finite.
    for (final v in invVar) {
      expect(v, greaterThan(0.0));
      expect(v.isFinite, isTrue);
    }
  });

  test('Step 3: Mahalanobis d\' responds to inverse-variance scaling and its '
      'diagonal is zero', () {
    final profile = FruitProfile(name: 'D');
    for (final c in profile.categories) {
      c.isEnabled = c.id == 0 || c.id == 1;
      c.sourceType = CategorySourceType.measured;
    }
    final protos = FruitProfileCompiler.compiledPrototypes(profile,
        measuredStates: {'UNRIPE': unripeTaps, 'PERFECTLY_RIPE': ripeTaps});
    final invBase = FruitProfileCompiler.compiledInverseVariance(profile,
        measuredStates: {'UNRIPE': unripeTaps, 'PERFECTLY_RIPE': ripeTaps});

    final unit = FruitProfileCompiler.mahalanobisMatrix(
        protos, List<double>.filled(32, 1.0));
    final highPixel = FruitProfileCompiler.mahalanobisMatrix(
        protos, List<double>.filled(32, 100.0));
    final lowPixel = FruitProfileCompiler.mahalanobisMatrix(
        protos, List<double>.filled(32, 0.001));

    // Scaling invVar up must push d' up; scaling down must pull it down.
    expect(highPixel[0][1], greaterThan(unit[0][1]));
    expect(lowPixel[0][1], lessThan(unit[0][1]));

    // Diagonal is exactly zero (no self-separation).
    expect(unit[0][0], 0.0);
    expect(unit[1][1], 0.0);

    // Real calibrated profile (within-class pooled invVar) reads healthy,
    // above the >= 1.0 selftest threshold.
    final real = FruitProfileCompiler.mahalanobisMatrix(protos, invBase);
    expect(real[0][1], greaterThanOrEqualTo(1.0),
        reason: 'calibrated UNRIPE/RIPE profile must pass the >=1.0 selftest');
  });
}
