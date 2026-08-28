import 'dart:math' as math;

/// Firmware-faithful 28-D extraction from raw collected data — a direct
/// Dart port of datacollection/extract_28d.py so prototypes built in-app
/// match exactly what the device computes at inference time.
///
/// One measured session (a category) yields:
///   • hue_histogram[8] + chromatic_dispersion + volume_cm3  (ms_captured)
///   • N raw 512-sample tap waveforms                        (arm -> tap)
///
/// [extractTap] mirrors extract_28d.fft_features() (per-window DC removal,
/// hanning, rfft, band-power integration, spectral entropy) and
/// [buildState] aggregates taps + hue exactly like extract_sample() — the
/// per-band log-power amplitude correction (force-invariance) is applied to
/// the averaged bins so tap strength cancels.
class TrainingExtractor {
  TrainingExtractor._();

  static const samplingFreqHz = 8820.0;
  static const fftSize = 512;
  static const nFftBins = 15;
  static const f2Norm = 441000.0;
  static const epsLog = 1e-10;
  static const fftClampMin = -10.0;
  static const minMass23 = 10.0;
  static const maxMass23 = 300.0;
  static const adcMaxCounts = 4095.0;
  static const impactAmpFloor = 0.004;
  static const forceInvariant = true;

  static const List<double> fftCenters = [
    150.0, 250.0, 350.0, 450.0, 550.0, 650.0, 750.0, 850.0, 950.0,
    1100.0, 1300.0, 1500.0, 1700.0, 1900.0, 2100.0,
  ];

  /// Acoustic display band for the live spectrum = the range the firmware
  /// band-analysis actually covers (the 15 centres span 150 Hz–2.1 kHz).
  static const double minBandHz = 150.0;
  static const double maxBandHz = 2100.0;

  /// Nearest FFT bin index for a frequency (mirrors the firmware band maths).
  static int _freqToBin(double hz) =>
      (hz / _binResolution).round().clamp(0, fftSize ~/ 2);

  static const double _binResolution = samplingFreqHz / fftSize; // ~17.23 Hz

  /// hanning window of length [n] (numpy.hanning: sym=True by default).
  static List<double> _hanning(int n) => List<double>.generate(
      n, (i) => 0.5 * (1.0 - math.cos(2.0 * math.pi * i / (n - 1))));

  static final List<double> _hannWin = _hanning(fftSize);

  /// Per-tap acoustic features (extract_28d.fft_features).
  static ({List<double> fftBins, double entropy, double impactAmp}) extractTap(
      List<double> wave) {
    assert(wave.length == fftSize,
        'expected $fftSize samples, got ${wave.length}');
    final bp = _bandPowers(wave);

    final fftBins = List<double>.filled(nFftBins, 0);
    for (var b = 0; b < nFftBins; b++) {
      final center = fftCenters[b];
      final p = bp.rawPower[b];
      final v = math.log(p + epsLog) * (center * center) / f2Norm;
      fftBins[b] = math.max(v, fftClampMin);
    }

    var total = 0.0;
    for (final p in bp.rawPower) {
      total += p;
    }
    var entropy = 0.0;
    if (total >= 1e-15) {
      var e = 0.0;
      for (final p in bp.rawPower) {
        final pr = p / total;
        e += pr * math.log(pr);
      }
      entropy = -e / math.log(15.0);
      entropy = math.max(0.0, math.min(1.0, entropy));
    }

    return (fftBins: fftBins, entropy: entropy, impactAmp: bp.impactAmp);
  }

  /// Real full-resolution FFT spectrum for live plotting: log-magnitude over
  /// the firmware's acoustic band (150 Hz → 2.1 kHz).
  ///
  /// The 28-D model's [extractTap].fftBins compress the FFT into 15
  /// force-invariant-scaled, clamped values that saturate as a ramp — and a
  /// single-tap mean-normalized 15-point spline looks spiky/artificial. This
  /// returns the genuine per-bin log magnitude (|FFT[k]|, dense ~119 points)
  /// across the band the device actually analyses, so the live graph shows the
  /// true spectral envelope (smooth decay + resonant bumps) instead of a
  /// sparse, wiggle-prone estimate.
  static List<double> spectrumMagnitude(List<double> wave) {
    final bp = _bandPowers(wave);
    final power = bp.fullPower;
    final lo = _freqToBin(minBandHz);
    final hi = _freqToBin(maxBandHz);
    final mag = List<double>.generate(hi - lo, (i) {
      final p = power[lo + i];
      return math.log(math.sqrt(p) + epsLog);
    });
    return mag;
  }

  /// Shared per-tap FFT integration: DC removal, hanning, rfft, then the
  /// summed power over the Hann mainlobe (k-1..k+1) around each centre, plus
  /// the raw ADC deviation (impact amplitude). Returns the full per-bin power
  /// too (used by [spectrumMagnitude] for the live curve). Used by both
  /// [extractTap] (28-D state, force-invariant scaling) and [spectrumMagnitude]
  /// (visual shape).
  static ({List<double> rawPower, List<double> fullPower, double impactAmp})
      _bandPowers(List<double> wave) {
    final mean = wave.fold(0.0, (a, b) => a + b) / wave.length;
    var impactAmp = 0.0;
    final centered = List<double>.filled(fftSize, 0);
    for (var i = 0; i < fftSize; i++) {
      final c = wave[i] - mean;
      centered[i] = c;
      final a = c.abs();
      if (a > impactAmp) impactAmp = a;
    }
    final windowed = List<double>.filled(fftSize, 0);
    for (var i = 0; i < fftSize; i++) {
      windowed[i] = centered[i] * _hannWin[i];
    }

    final power = _rfftPower(windowed); // len 257

    final rawPower = List<double>.filled(nFftBins, 0);
    for (var b = 0; b < nFftBins; b++) {
      final center = fftCenters[b];
      var binIdx = (center / _binResolution).round();
      if (binIdx >= power.length) binIdx = power.length - 1;
      var lo = binIdx - 1;
      if (lo < 0) lo = 0;
      var hi = binIdx + 1;
      if (hi > power.length - 1) hi = power.length - 1;
      var p = 0.0;
      for (var k = lo; k <= hi; k++) {
        p += power[k];
      }
      rawPower[b] = p;
    }
    return (rawPower: rawPower, fullPower: power, impactAmp: impactAmp);
  }

  /// Aggregate taps + hue into the 28-D prototype state.
  ///
  /// Mirrors extract_28d.extract_sample(): hue dims averaged from hue JSONs,
  /// then FFT bins/entropy/impact averaged across taps, then the
  /// force-invariant amplitude correction applied to the averaged bins.
  static List<double> buildState({
    required List<double> hueHistogram,
    required double chromaticDispersion,
    required double volumeCm3,
    required List<({List<double> fftBins, double entropy, double impactAmp})>
        taps,
  }) {
    assert(hueHistogram.length == 8);

    final n = taps.isEmpty ? 1 : taps.length;
    var fftAcc = List<double>.filled(nFftBins, 0);
    var entAcc = 0.0;
    var peakAcc = 0.0;
    for (final t in taps) {
      for (var b = 0; b < nFftBins; b++) {
        fftAcc[b] += t.fftBins[b];
      }
      entAcc += t.entropy;
      peakAcc += t.impactAmp;
    }
    final fftAvg = fftAcc.map((v) => v / n).toList();
    final entAvg = entAcc / n;
    final peakAvg = peakAcc / n;

    final state = List<double>.filled(28, 0);
    state.setRange(0, 8, hueHistogram);
    state[8] = chromaticDispersion;
    final vol23 = math.pow(volumeCm3, 2.0 / 3.0).toDouble();
    state[9] = _clip((vol23 - minMass23) / (maxMass23 - minMass23), 0.0, 1.0);

    if (forceInvariant) {
      final amp = math.max(peakAvg / adcMaxCounts, impactAmpFloor);
      final corr = 2.0 * math.log(amp);
      for (var b = 0; b < nFftBins; b++) {
        state[10 + b] =
            fftAvg[b] - corr * (fftCenters[b] * fftCenters[b]) / f2Norm;
      }
      state[26] = 0.0; // class-neutral under force invariance
    } else {
      state.setRange(10, 25, fftAvg);
      state[26] = peakAvg / adcMaxCounts;
    }
    state[25] = entAvg;
    state[27] = 0.0;
    return state;
  }

  static double _clip(double v, double lo, double hi) =>
      v < lo ? lo : (v > hi ? hi : v);

  // ── Radix-2 real FFT (power of two only) → power spectrum ───────────
  /// Returns |FFT[k]|^2 for k in 0..N/2 (the rfft-positive half), computed
  /// on the complex FFT of the real input (imaginary part zero).
  static List<double> _rfftPower(List<double> real) {
    final n = real.length;
    assert(_isPow2(n), 'FFT size must be a power of two');

    // Pack real into real array, zero imag.
    final re = List<double>.from(real);
    final im = List<double>.filled(n, 0);
    _fft(re, im, n);

    final half = n ~/ 2 + 1; // rfft positive frequencies 0..N/2
    final power = List<double>.filled(half, 0);
    for (var k = 0; k < half; k++) {
      power[k] = re[k] * re[k] + im[k] * im[k];
    }
    return power;
  }

  static bool _isPow2(int n) => n > 0 && (n & (n - 1)) == 0;

  /// In-place iterative radix-2 Cooley-Tukey FFT on [re]/[im].
  static void _fft(List<double> re, List<double> im, int n) {
    // bit-reversal permutation
    for (var i = 1, j = 0; i < n; i++) {
      var bit = n >> 1;
      for (; j & bit > 0; bit >>= 1) {
        j ^= bit;
      }
      j ^= bit;
      if (i < j) {
        final tr = re[i];
        re[i] = re[j];
        re[j] = tr;
        final ti = im[i];
        im[i] = im[j];
        im[j] = ti;
      }
    }

    for (var len = 2; len <= n; len <<= 1) {
      final angle = -2.0 * math.pi / len;
      final wRe = math.cos(angle);
      final wIm = math.sin(angle);
      for (var i = 0; i < n; i += len) {
        var cRe = 1.0, cIm = 0.0;
        final half = len >> 1;
        for (var k = 0; k < half; k++) {
          final uRe = re[i + k], uIm = im[i + k];
          final vRe = re[i + k + half] * cRe - im[i + k + half] * cIm;
          final vIm = re[i + k + half] * cIm + im[i + k + half] * cRe;
          re[i + k] = uRe + vRe;
          im[i + k] = uIm + vIm;
          re[i + k + half] = uRe - vRe;
          im[i + k + half] = uIm - vIm;
          final ncRe = cRe * wRe - cIm * wIm;
          final ncIm = cRe * wIm + cIm * wRe;
          cRe = ncRe;
          cIm = ncIm;
        }
      }
    }
  }
}
