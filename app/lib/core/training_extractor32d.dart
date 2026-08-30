import 'dart:math' as math;

/// Firmware-faithful 32-D extraction from raw collected data — a direct Dart
/// port of tests/test_fruit_profile_engine.py `extract_state_32d` +
/// `extract_state_32d_balanced` (and the device `assemble_state_32d` in
/// esp/main/sci_32d.cpp + `build_stft_grid`/`compute_bio_moments` in
/// piezo_acoustic.cpp).
///
/// One measured session (a category) yields:
///   • hue_histogram[8] + dispersion + volume_cm3   (ms_captured)
///   • N raw 512-sample tap waveforms               (arm -> tap)
///
/// The 32-D prototype for a class is the mean of [extractState32dBalanced]
/// across every tap, exactly like fit_prototypes' centroid. Because the audio
/// image + bio-moments are already scale-free (block-normalized), tap
/// amplitude/force cancels out automatically.
class TrainingExtractor32D {
  TrainingExtractor32D._();

  static const double fs = 8820.0;
  static const int nSamples = 512;

  // STFT geometry (must match firmware constants).
  static const int nFft = 128;
  static const int hop = 32;
  static const int window = 384;
  static const int nBands = 4;
  static const int nFrames = 9; // (384-128)/32 + 1

  static const List<double> freqBandsLo = [100.0, 250.0, 500.0, 850.0];
  static const List<double> freqBandsHi = [250.0, 500.0, 850.0, 1400.0];

  static const double volumeMinCm3 = 10.0;
  static const double volumeMaxCm3 = 600.0;
  static const double vol23Min = 4.64; // (10)^(2/3)
  static const double vol23Max = 71.13; // (600)^(2/3)

  static const double contrastFloorDb = 1.5;
  static const double noiseGate = 0.05;
  static const double dampingScale = 10.0;
  static const double harmonicClip = 3.0;
  static const int lateWindowSamples = 176; // int(0.020 * 8820)
  static const int rmsEnvWindow = 16;

  // ── Optics & mass (0..9) ───────────────────────────────────────────
  static List<double> _optics(List<double> hue8, double volumeCm3) {
    final h = List<double>.filled(8, 0.0);
    for (var i = 0; i < 8; i++) {
      h[i] =
          (i < hue8.length && hue8[i].isFinite) ? hue8[i] : 0.0;
    }
    var varSum = 0.0;
    for (var i = 0; i < 8; i++) {
      final d = h[i] - 1.0 / 8.0;
      varSum += d * d;
    }
    final disp = _clamp(1.0 - varSum / 0.875, 0.0, 1.0);

    final vol = volumeCm3.clamp(volumeMinCm3, volumeMaxCm3);
    final vol23 = math.pow(vol, 2.0 / 3.0).toDouble();
    final vNorm =
        _clamp((vol23 - vol23Min) / (vol23Max - vol23Min), 0.0, 1.0);

    return [
      ...h,
      disp,
      vNorm,
    ];
  }

  // ── STFT 4x4 grid (10..25) ────────────────────────────────────────
  /// Builds the peak-aligned 4x4 time-frequency grid from a 512-tap waveform.
  ///
  /// Mirrors build_stft_grid: frame count = 9; per-frame linear band power is
  /// pooled per time-slice, then `log1p(sum)` is applied once per (t,band).
  static List<double> _stftGrid(List<double> centered) {
    // Peak-align.
    var peakIdx = 0;
    var peakVal = -1.0;
    for (var i = 0; i < centered.length; i++) {
      final a = centered[i].abs();
      if (a > peakVal) {
        peakVal = a;
        peakIdx = i;
      }
    }
    var startIdx = (peakIdx - 16).clamp(0, centered.length - window);
    final xa = List<double>.filled(window, 0.0);
    for (var i = 0; i < window; i++) {
      xa[i] = centered[startIdx + i];
    }

    // Hanning window (numpy.sym=True).
    final win = List<double>.generate(nFft,
        (i) => 0.5 * (1.0 - math.cos(2.0 * math.pi * i / (nFft - 1))));

    // Frame linear band power (before log1p — pooling happens linearly).
    final frameLin = List<List<double>>.generate(
        nFrames, (_) => List<double>.filled(nBands, 0.0));
    for (var s = 0; s <= window - nFft; s += hop) {
      final frame = s ~/ hop;
      // Segment * window, then rfft magnitude^2.
      final seg = List<double>.generate(
          nFft, (i) => xa[s + i] * win[i]);
      final power = _rfftPower(seg); // len 65
      for (var b = 0; b < nBands; b++) {
        var sum = 0.0;
        for (var k = 0; k < 65; k++) {
          final f = k * fs / nFft;
          if (f >= freqBandsLo[b] && f < freqBandsHi[b]) {
            sum += power[k];
          }
        }
        frameLin[frame][b] = sum;
      }
    }

    // Pool frames into 4 time slices.
    const framesPerSlice = nFrames ~/ 4; // 2
    final grid = List.generate(4, (_) => List<double>.filled(4, 0.0));
    for (var t = 0; t < 4; t++) {
      final tStart = t * framesPerSlice;
      final tEnd = (t < 3) ? (t + 1) * framesPerSlice : nFrames;
      for (var b = 0; b < nBands; b++) {
        var sum = 0.0;
        for (var fr = tStart; fr < tEnd; fr++) {
          sum += frameLin[fr][b];
        }
        grid[t][b] = math.log(1.0 + sum);
      }
    }

    // Bounded contrast normalization (1.5 dB floor).
    var gMin = grid[0][0], gMax = grid[0][0];
    for (var t = 0; t < 4; t++) {
      for (var b = 0; b < 4; b++) {
        if (grid[t][b] < gMin) gMin = grid[t][b];
        if (grid[t][b] > gMax) gMax = grid[t][b];
      }
    }
    final dyn = math.max(gMax - gMin, contrastFloorDb);
    final out = List<double>.filled(16, 0.0);
    for (var t = 0; t < 4; t++) {
      for (var b = 0; b < 4; b++) {
        out[t * 4 + b] = _clamp((grid[t][b] - gMin) / dyn, 0.0, 1.0);
      }
    }
    return out;
  }

  // ── Bio-moments (26..31) ──────────────────────────────────────────
  static List<double> _bioMoments(
      List<double> centered, List<double> normGrid, double amp,
      List<List<double>> stftMatrix, double state9) {
    // x_norm = centered / amp; e_t = x_norm^2
    final e_t = List<double>.generate(centered.length, (i) {
      final xn = centered[i] / amp;
      return xn * xn;
    });
    var totalEnergy = 0.0;
    for (final e in e_t) {
      totalEnergy += e;
    }
    totalEnergy += 1e-6;

    // Dim 26: temporal centroid.
    var num26 = 0.0;
    for (var i = 0; i < e_t.length; i++) {
      final tw = i / (e_t.length - 1);
      num26 += tw * e_t[i];
    }
    final centroid = num26 / totalEnergy;

    // Dim 27: tail energy after 20 ms.
    final late = lateWindowSamples.clamp(0, e_t.length);
    var lateSum = 0.0;
    for (var i = late; i < e_t.length; i++) {
      lateSum += e_t[i];
    }
    final tail = lateSum / totalEnergy;

    // Dim 28: harmonic absorption from NORMALIZED grid.
    var fund = 0.0, harm = 0.0;
    for (var t = 0; t < 4; t++) {
      fund += normGrid[t * 4 + 0];
    }
    for (var t = 0; t < 4; t++) {
      harm += normGrid[t * 4 + 2] + normGrid[t * 4 + 3];
    }
    fund += 1e-5;
    harm += 1e-5;
    final harmonic = _clamp(harm / fund, 0.0, harmonicClip) / harmonicClip;

    // Dim 29: abbott stiffness proxy (mean fundamental grid energy x volume-norm).
    var fundGrid = 0.0;
    for (var t = 0; t < 4; t++) {
      fundGrid += normGrid[t * 4 + 0];
    }
    // Normalize across the 4 time slices so fundamental energy spans [0,1]
    // instead of saturating at the 1.0 clamp (fundGrid alone reaches ~4.0).
    final abbott = _clamp((fundGrid / 4.0) * state9, 0.0, 1.0);

    // Dim 30: spectral entropy over the mean STFT PSD.
    const specLen = nFft ~/ 2 + 1; // 65
    final rawPsd = List<double>.filled(specLen, 1e-8);
    final nF = stftMatrix.length;
    if (nF > 0) {
      for (var k = 0; k < specLen; k++) {
        var sum = 0.0;
        for (var f = 0; f < nF; f++) {
          sum += stftMatrix[f][k];
        }
        rawPsd[k] = sum / nF + 1e-8;
      }
    }
    var psdSum = 0.0;
    for (final v in rawPsd) {
      psdSum += v;
    }
    var entropy = 0.0;
    if (psdSum > 1e-12) {
      var H = 0.0;
      for (final v in rawPsd) {
        final p = v / psdSum;
        if (p > 1e-15) H -= p * math.log(p);
      }
      entropy = H / math.log(specLen.toDouble());
    }

    // Dim 31: dynamic damping (evenly-spaced x over valid points).
    final rmsLen = e_t.length - rmsEnvWindow + 1;
    final rmsEnv = List<double>.filled(rmsLen, 0.0);
    for (var i = 0; i < rmsLen; i++) {
      var acc = 0.0;
      for (var j = 0; j < rmsEnvWindow; j++) {
        acc += e_t[i + j];
      }
      rmsEnv[i] = math.sqrt(acc / rmsEnvWindow + 1e-7);
    }
    var peakRms = 0;
    for (var i = 1; i < rmsLen; i++) {
      if (rmsEnv[i] > rmsEnv[peakRms]) peakRms = i;
    }
    var damping = 0.5;
    final ys = <double>[];
    for (var i = peakRms; i < rmsLen; i++) {
      if (rmsEnv[i] > noiseGate) ys.add(math.log(rmsEnv[i]));
    }
    if (ys.length > 8) {
      var sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
      for (var k = 0; k < ys.length; k++) {
        final x = (ys.length > 1) ? k / (ys.length - 1) : 0.0;
        final y = ys[k];
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
      }
      final n = ys.length.toDouble();
      final denom = n * sxx - sx * sx;
      var slope = 0.0;
      if (denom.abs() > 1e-12) {
        slope = (n * sxy - sx * sy) / denom;
      }
      damping = _clamp(-slope / dampingScale, 0.0, 1.0);
    }

    return [centroid, tail, harmonic, abbott, entropy, damping];
  }

  // ── Public entry points ───────────────────────────────────────────
  /// Raw (pre-block-normalized) 32-D state. Mirrors extract_state_32d.
  static List<double> extractState32d({
    required List<double> wave,
    List<double>? visionHues,
    double volumeCm3 = 350.0,
  }) {
    assert(wave.length == nSamples,
        'expected $nSamples samples, got ${wave.length}');

    final x = List<double>.from(wave);
    var mean = 0.0;
    for (final v in x) {
      mean += v;
    }
    mean /= x.length;
    final centered = List<double>.generate(x.length, (i) => x[i] - mean);

    // High-pass filter above ~80 Hz (1st-order, alpha 0.945 @ fs 8820) to strip
    // the ~17 Hz transducer electrical discharge that otherwise dominates the
    // acoustic band (98-99% of energy saturates below 150 Hz). The filter input
    // is the DC-removed signal; the differentiator term cancels the residual DC.
    final filtered = List<double>.filled(centered.length, 0.0);
    const double hpAlpha = 0.945;
    for (var i = 1; i < centered.length; i++) {
      filtered[i] =
          hpAlpha * (filtered[i - 1] + centered[i] - centered[i - 1]);
    }

    var amp = 0.0;
    for (final v in filtered) {
      final a = v.abs();
      if (a > amp) amp = a;
    }
    amp += 1e-6;

    final state = List<double>.filled(32, 0.0);

    // Optics & mass.
    final opt = _optics(visionHues ?? const [], volumeCm3);
    state.setRange(0, 10, opt);

    // STFT grid + matrix (matrix needed for entropy).
    final normGrid = _stftGrid(filtered);
    state.setRange(10, 26, normGrid);

    // Recompute full frame power matrix for spectral entropy.
    final stftMatrix = _stftMatrixPower(filtered);

    final bio = _bioMoments(filtered, normGrid, amp, stftMatrix, state[9]);
    state.setRange(26, 32, bio);

    return state;
  }

  /// Block-L2-normalized 32-D state (force-invariantly balanced).
  /// Mirrors extract_state_32d_balanced / assemble_state_32d.
  static List<double> extractState32dBalanced({
    required List<double> wave,
    List<double>? visionHues,
    double volumeCm3 = 350.0,
  }) {
    final s = extractState32d(
        wave: wave, visionHues: visionHues, volumeCm3: volumeCm3);
    _blockL2(s, 0, 10);
    _blockL2(s, 10, 16);
    _blockL2(s, 26, 6);
    return s;
  }

  /// Full per-frame STFT power matrix (9 x 65), used for spectral entropy.
  static List<List<double>> _stftMatrixPower(List<double> centered) {
    var peakIdx = 0;
    var peakVal = -1.0;
    for (var i = 0; i < centered.length; i++) {
      final a = centered[i].abs();
      if (a > peakVal) {
        peakVal = a;
        peakIdx = i;
      }
    }
    final startIdx = (peakIdx - 16).clamp(0, centered.length - window);
    final xa = List<double>.generate(
        window, (i) => centered[startIdx + i]);
    final win = List<double>.generate(nFft,
        (i) => 0.5 * (1.0 - math.cos(2.0 * math.pi * i / (nFft - 1))));

    final out = <List<double>>[];
    for (var s = 0; s <= window - nFft; s += hop) {
      final seg =
          List<double>.generate(nFft, (i) => xa[s + i] * win[i]);
      out.add(_rfftPower(seg));
    }
    return out;
  }

  static void _blockL2(List<double> s, int off, int len) {
    var n2 = 0.0;
    for (var i = 0; i < len; i++) {
      n2 += s[off + i] * s[off + i];
    }
    final inv = 1.0 / (math.sqrt(n2) + 1e-6);
    for (var i = 0; i < len; i++) {
      s[off + i] *= inv;
    }
  }

  static double _clamp(double v, double lo, double hi) =>
      v < lo ? lo : (v > hi ? hi : v);

  // ── FFT (mirrors the firmware dsps_fft2r_* via numpy rfft) ─────────
  /// |FFT[k]|^2 for k in 0..N/2 of a real input (rfft positive half).
  static List<double> _rfftPower(List<double> real) {
    final n = real.length;
    final re = List<double>.from(real);
    final im = List<double>.filled(n, 0.0);
    _fft(re, im, n);
    final half = n ~/ 2 + 1;
    return List<double>.generate(
        half, (k) => re[k] * re[k] + im[k] * im[k]);
  }

  static void _fft(List<double> re, List<double> im, int n) {
    for (var i = 1, j = 0; i < n; i++) {
      var bit = n >> 1;
      for (; (j & bit) > 0; bit >>= 1) {
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
