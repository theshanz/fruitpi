#include "piezo_acoustic.h"
#include "driver/adc.h"
#include "dsps_fft2r.h"
#include "esp_err.h"
#include "hal/adc_types.h"
#include "sdkconfig.h"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <esp32-hal-adc.h>
#include <esp32-hal.h>
#include <pgmspace.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_task.h"


PiezoAcoustic::PiezoAcoustic(adc1_channel_t channel) : adc_channel(channel){}

void PiezoAcoustic::generate_hanning_window(){
    for(int i = 0; i<FFT_SIZE; i++){
        hanning_window[i] = 0.5f * (1.0f - cosf((2.0f * M_PI * i) / (FFT_SIZE - 1)));
    }
}

bool PiezoAcoustic::init(){
    //config the adc channel
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(adc_channel, ADC_ATTEN_DB_12);

    //init esp-dsp hardware
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    if (ret != ESP_OK){
        Serial.printf("[Piezo] Error initializing esp-dsp FFT: %d\n", ret);
        return false;
    }

    generate_hanning_window();

    // Create mutex for thread-safe feature retrieval
    data_mutex = xSemaphoreCreateMutex();
    if (data_mutex == NULL) {
        Serial.println("[Piezo] Error creating data mutex!");
        return false;
    }

    // Spawn background continuous sampling task on Core 1 (off the WiFi/BT
    // radio core so BT traffic can't preempt it and punch sampling gaps).
    xTaskCreatePinnedToCore(
        PiezoAcoustic::sampler_task_code,
        "PiezoSamplerTask",
        6144,
        this,
        10,                      // High priority
        &sampler_task_handle,
        1                        // Core 1
    );

    Serial.println("[Piezo] Hardware FFT & Background Sampler Subsystem Initialized.");
    return true;
}

//Coumpute Normalized Spectral Entropy math
float PiezoAcoustic::compute_spectral_entropy(const float raw_power[N_FFT_BINS]){
    float sum = 0.0f;
    for (int i = 0; i< N_FFT_BINS; i++){
        sum += raw_power[i];
    }

    //silance check
    if (sum < 1e-15f) return 0.0f;

    float inv_sum = 1.0f / sum;
    float H = 0.0f;

    for (int i = 0; i < N_FFT_BINS; i++){
        float p = raw_power[i] * inv_sum;
        if (p > 1e-15f) {
             H -= p * logf(p); // Shannon Entropy formula
         }
    }

    float log15 = 2.70805f; // log(15) value
    float normalized_entropy = H / log15;

    //clamping
    if (normalized_entropy < 0.0f) return 0.0f;
    if (normalized_entropy > 1.0f) return 1.0f;
    return normalized_entropy;
}

void PiezoAcoustic::capture_raw_waveform(uint16_t raw_out[512], uint16_t* peak_out){
    uint16_t max_peak = 0;
    uint32_t sample_period_us = 1000000 / SAMPLING_FREQ_HZ; // 113
    for (int i = 0; i < 512; i++){
        uint32_t t_start = micros();

        uint16_t raw_adc = (uint16_t)adc1_get_raw(adc_channel);

        if(raw_adc > max_peak) {
            max_peak = raw_adc;
        }

        raw_out[i] = raw_adc;

        while ((micros() - t_start) < sample_period_us);
    }

    if(peak_out != nullptr) {
        *peak_out = max_peak;
    }
}

// adc_buffer is written by Core-1 in process_captured_buffer() and read here.
// Both paths are guarded by protocol: callers only reach this after is_data_ready()
// returns true, meaning the sampler has finished processing and is back in DISARMED.
// data_mutex protects latest_features; adc_buffer safety relies on this state convention.
bool PiezoAcoustic::capture_sampler_raw_window(uint16_t raw_out[512], uint16_t* peak_out) {
    if (xSemaphoreTake(data_mutex, portMAX_DELAY)) {
        uint16_t peak = 0;
        for (int i = 0; i < FFT_SIZE; i++) {
            int raw = (int)lroundf(adc_buffer[i] * 4095.0f);
            if (raw < 0) raw = 0;
            if (raw > 4095) raw = 4095;
            raw_out[i] = (uint16_t)raw;
            if (raw > peak) peak = (uint16_t)raw;
        }
        if (peak_out != nullptr) {
            *peak_out = peak;
        }
        new_data_available.store(false);
        xSemaphoreGive(data_mutex);
        return true;
    }
    return false;
}

//////////////////Continuous Background Recording Subsystem Methods /////////////////////////////////////

void PiezoAcoustic::arm(float trigger_threshold, uint32_t settle_us) {
    if (xSemaphoreTake(data_mutex, portMAX_DELAY)) {
        trigger_threshold_adc = trigger_threshold;
        this->settle_us = settle_us;
        new_data_available.store(false);
        post_trigger_counter = 0;
        samples_since_arm = 0;
        // Keep last_normalized_val at previous ambient reading so first-sample
        // delta is tiny (~0.002) instead of spiking to 0.032 from 0.0 init.

        //prevent old data on new arm sequence
        memset(ring_buffer, 0, sizeof(ring_buffer));
        ring_head = 0;

        armed_start_us = micros();
        state.store(STATE_ARMED);
        xSemaphoreGive(data_mutex);
    }
    if (sampler_task_handle != nullptr) {
        xTaskNotifyGive(sampler_task_handle);   // wake sampler immediately
    }
}

void PiezoAcoustic::disarm() {
    state.store(STATE_DISARMED);
}

bool PiezoAcoustic::is_data_ready() {
    return new_data_available.load();
}

SamplingState PiezoAcoustic::get_state() {
    return state.load();
}

AcousticFeatures PiezoAcoustic::get_latest_features() {
    AcousticFeatures temp = {0};
    if (xSemaphoreTake(data_mutex, portMAX_DELAY)) {
        temp = latest_features;
        new_data_available.store(false);
        xSemaphoreGive(data_mutex);
    }
    return temp;
}

// FreeRTOS Task: High-priority background loop on Core 1
void PiezoAcoustic::sampler_task_code(void* parameter) {
    PiezoAcoustic* self = static_cast<PiezoAcoustic*>(parameter);
    uint32_t sample_period_us = 1000000 / SAMPLING_FREQ_HZ; // ~113 us

    while (true) {
        while (self->state.load() == STATE_DISARMED)     // sleep until armed
          ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t t_start = micros();

        int raw_adc = adc1_get_raw(self->adc_channel);
        float normalized_val = (float)raw_adc / 4095.0f;

        // 1. State evaluation prep.
        SamplingState cur_state = self->state.load();
        const bool flushing =
            (cur_state == STATE_ARMED) &&
            (self->samples_since_arm < PIEZO_FLUSH_SAMPLES);

        // 2. Ring write — but NOT while flushing: right after arming the
        // analog stage may still carry residual charge / ring from an event
        // that happened while disarmed (tap during an idle gap) or during
        // settle. Those samples are discarded entirely so a capture window
        // can never open mid-decay of a stale event.
        if (!flushing) {
            uint16_t current_head = self->ring_head;
            self->ring_buffer[current_head] = normalized_val;
            self->ring_head = (current_head + 1) % RING_BUFFER_SIZE;
        }

        if (cur_state == STATE_ARMED) {
            self->samples_since_arm++;

            if (flushing) {
                // Bleed-off phase: track ambient fast so the baseline locks
                // onto post-flush reality, no trigger checks, no ring writes.
                self->baseline +=
                    BASELINE_ALPHA_SETTLE * (normalized_val - self->baseline);
                self->prev_sample_val = normalized_val;
            } else {
                const bool settling =
                    (micros() - self->armed_start_us) < self->settle_us;

                // Settle hold-off: button press / fruit placement / LCD burst
                // rings the piezo right after arming — keep tracking ambient
                // and ignore triggers until it decays, otherwise a tap is
                // "detected" instantly.
                if (settling) {
                    self->baseline +=
                        BASELINE_ALPHA_SETTLE * (normalized_val - self->baseline);
                    self->prev_sample_val = normalized_val;
                } else {
                    // Slow ambient tracking while listening: rail sag / drift
                    // moves the baseline, not the trigger.
                    self->baseline +=
                        BASELINE_ALPHA_LISTEN * (normalized_val - self->baseline);

                    float delta = normalized_val - self->prev_sample_val;
                    if (delta < 0) delta = -delta;
                    self->prev_sample_val = normalized_val;

                    float dev = normalized_val - self->baseline;
                    if (dev < 0) dev = -dev;

                    // Adaptive slew gate: track ambient |delta| noise and
                    // demand several times that, floored at the static
                    // minimum and capped so moderate taps always pass.
                    self->slew_noise_ema +=
                        SLEW_NOISE_ALPHA * (delta - self->slew_noise_ema);
                    float slew_gate = SLEW_GATE_MULT * self->slew_noise_ema;
                    if (slew_gate < TRIGGER_DELTA_MIN) slew_gate = TRIGGER_DELTA_MIN;
                    if (slew_gate > SLEW_GATE_MAX) slew_gate = SLEW_GATE_MAX;

                    // Adaptive deviation gate: same trick against persistent
                    // interference floors (LCD ripple, ground bounce).
                    self->dev_noise_ema +=
                        DEV_NOISE_ALPHA * (dev - self->dev_noise_ema);
                    float dev_gate = DEV_GATE_MULT * self->dev_noise_ema;
                    if (dev_gate < self->trigger_threshold_adc)
                        dev_gate = self->trigger_threshold_adc;
                    if (dev_gate > DEV_GATE_MAX) dev_gate = DEV_GATE_MAX;

                    // Require a sharp impulse above the noise-adaptive slew
                    // gate AND real energy above the deviation gate.
                    if (delta >= slew_gate && dev >= dev_gate) {
                        self->post_trigger_counter = 0;
                        self->state.store(STATE_CAPTURING);
                        Serial.printf("[PZ] TAP s=%u d=%.3f g=%.3f v=%.3f vg=%.3f\n",
                                      self->samples_since_arm, delta,
                                      slew_gate, dev, dev_gate);
                    }
                }
            }
            // Auto-disarm if no tap within timeout (bounds busy window)
            if (micros() - self->armed_start_us >= ARM_TIMEOUT_US) {
                Serial.printf("[PZ] timeout, noise_slew=%.4f\n",
                              self->slew_noise_ema);
                self->state.store(STATE_DISARMED);
            }
        }
        else if (cur_state == STATE_CAPTURING) {
            self->post_trigger_counter++;
            uint16_t required_post = FFT_SIZE - PRE_TRIGGER_SAMPLES;

            if (self->post_trigger_counter >= required_post) {
                self->state.store(STATE_PROCESSING);

                // 3. Reconstruct full window containing 64 pre-trigger + 448 post-trigger samples
                uint16_t start_idx = (self->ring_head + RING_BUFFER_SIZE - FFT_SIZE) % RING_BUFFER_SIZE;
                for (int i = 0; i < FFT_SIZE; i++) {
                    self->adc_buffer[i] = self->ring_buffer[(start_idx + i) % RING_BUFFER_SIZE];
                }

                // Run FFT DSP pipeline on background core
                self->process_captured_buffer();

                // Disarm after capture completion
                self->state.store(STATE_DISARMED);
            }
        }

        // Precise microsecond timing enforcement
        while ((micros() - t_start) < sample_period_us);

        // Yield so lower-priority tasks (Arduino loop, camera) on Core 1 still
        // get CPU while the sampler is armed and busy-waiting.
        taskYIELD();
    }
}

// ─── 32D Fruit Profile DSP: STFT grid + bio-moments ───────────────
void PiezoAcoustic::build_stft_grid(
    const float centered[FFT_SIZE],
    float out[16],
    float raw_psd_mean[STFT_FFT_N / 2 + 1])
{
    // Lazily build the 128-point Hanning window once.
    static bool win_ready = false;
    if (!win_ready) {
        for (int i = 0; i < STFT_FFT_N; i++) {
            stft_win[i] = 0.5f * (1.0f - cosf((2.0f * (float)M_PI * i) /
                                                (STFT_FFT_N - 1)));
        }
        win_ready = true;
    }

    // Peak-align the 384-sample window exactly like the validated engine:
    //   peak at index 16 within the aligned window (16 pre / 368 post).
    int peak_idx = 0;
    float peak_amp = fabsf(centered[0]);
    for (int i = 1; i < FFT_SIZE; i++) {
        float a = fabsf(centered[i]);
        if (a > peak_amp) { peak_amp = a; peak_idx = i; }
    }
    int start_idx = peak_idx - STFT_ALIGN_PRE;
    int max_start = FFT_SIZE - STFT_WINDOW;
    if (start_idx < 0) start_idx = 0;
    if (start_idx > max_start) start_idx = max_start;

    float aligned[STFT_WINDOW] = {0};
    for (int i = 0; i < STFT_WINDOW; i++) aligned[i] = centered[start_idx + i];

    // Mean raw PSD across all STFT frames (for entropy / harmonic moments).
    const int spec_len = STFT_FFT_N / 2 + 1;
    for (int i = 0; i < spec_len; i++) raw_psd_mean[i] = 0.0f;
    // Per-frame linear band power (pooled together before log1p, matching
    // the validated engine's np.log1p(np.sum(..., axis=time))).
    float frame_lin[STFT_FRAMES][STFT_FREQ_BANDS] = {{0}};

    int n_frames = 0;
    for (int s = 0; s + STFT_FFT_N <= STFT_WINDOW; s += STFT_HOP) {
        for (int i = 0; i < STFT_FFT_N; i++) {
            stft_fft[i * 2]     = aligned[s + i] * stft_win[i];
            stft_fft[i * 2 + 1] = 0.0f;
        }
#if defined(CONFIG_IDF_TARGET_ESP32S3)
        dsps_fft2r_fc32_ae32(stft_fft, STFT_FFT_N);
#else
        dsps_fft2r_fc32(stft_fft, STFT_FFT_N);
#endif
        dsps_bit_rev_fc32(stft_fft, STFT_FFT_N);

        float bin_res = (float)SAMPLING_FREQ_HZ / (float)STFT_FFT_N;
        for (int b = 0; b < spec_len; b++) {
            float real = stft_fft[b * 2];
            float imag = stft_fft[b * 2 + 1];
            raw_psd_mean[b] += (real * real) + (imag * imag);
        }

        for (int fb = 0; fb < STFT_FREQ_BANDS; fb++) {
            float sum = 0.0f;
            // Match the validated engine's exact band mask fb in [lo, hi):
            // first bin with freq>=lo is ceil(lo/bin_res); first bin with
            // freq>=hi is ceil(hi/bin_res) (excluded). Truncation would pull
            // in the DC-neighbour bin and inflate the fundamental band.
            int k0 = (int)ceilf(STFT_BAND_HZ[fb][0] / bin_res);
            int k1 = (int)ceilf(STFT_BAND_HZ[fb][1] / bin_res);
            if (k1 > spec_len - 1) k1 = spec_len - 1;
            for (int kk = k0; kk < k1; kk++) {
                float real = stft_fft[kk * 2];
                float imag = stft_fft[kk * 2 + 1];
                sum += (real * real) + (imag * imag);
            }
            frame_lin[n_frames][fb] = sum;
        }
        n_frames++;
    }

    for (int i = 0; i < spec_len; i++) raw_psd_mean[i] /= (float)n_frames;

    // Pool frames into a 4x4 grid (groups of frames_per_slice along time),
    // summing LINEAR power, then apply log1p to each pooled tile.
    int frames_per_slice = n_frames / 4;
    float grid[4][4] = {{0}};
    for (int t = 0; t < 4; t++) {
        int t_start = t * frames_per_slice;
        int t_end = (t < 3) ? (t + 1) * frames_per_slice : n_frames;
        for (int f = t_start; f < t_end; f++) {
            for (int fb = 0; fb < 4; fb++) grid[t][fb] += frame_lin[f][fb];
        }
    }
    for (int t = 0; t < 4; t++)
        for (int fb = 0; fb < 4; fb++)
            grid[t][fb] = logf(1.0f + grid[t][fb]);

    // Bounded contrast normalization (1.5 dB floor), flattened row-major.
    float g_min = grid[0][0], g_max = grid[0][0];
    for (int t = 0; t < 4; t++)
        for (int fb = 0; fb < 4; fb++) {
            if (grid[t][fb] < g_min) g_min = grid[t][fb];
            if (grid[t][fb] > g_max) g_max = grid[t][fb];
        }
    float dyn = (g_max - g_min);
    if (dyn < STFT_CONTRAST_DB) dyn = STFT_CONTRAST_DB;
    for (int t = 0; t < 4; t++)
        for (int fb = 0; fb < 4; fb++)
            out[t * 4 + fb] = (grid[t][fb] - g_min) / dyn;
}

void PiezoAcoustic::compute_bio_moments(
    const float centered[FFT_SIZE],
    const float grid[16],
    const float raw_psd_mean[STFT_FFT_N / 2 + 1],
    float volume_norm,
    float moments[6])
{
    // Normalized energy envelope (member buffers keep the sampler task stack low).
    float amp = 0.0f;
    for (int i = 0; i < FFT_SIZE; i++) {
        float a = fabsf(centered[i]);
        if (a > amp) amp = a;
    }
    if (amp < 1e-6f) amp = 1e-6f;

    float* e_t = dsp_energy;
    float total_energy = 0.0f;
    for (int i = 0; i < FFT_SIZE; i++) {
        float xn = centered[i] / amp;
        e_t[i] = xn * xn;
        total_energy += e_t[i];
    }
    total_energy += 1e-6f;

    // Dim 26: temporal centroid (0..1).
    float num = 0.0f, den = 0.0f;
    for (int i = 0; i < FFT_SIZE; i++) {
        float tw = (float)i / (float)(FFT_SIZE - 1);
        num += tw * e_t[i];
        den += e_t[i];
    }
    moments[0] = num / (den + 1e-6f);

    // Dim 27: tail energy ratio (energy after 20 ms).
    int late_idx = (int)(TAIL_WINDOW_S * SAMPLING_FREQ_HZ);
    if (late_idx > FFT_SIZE) late_idx = FFT_SIZE;
    float late = 0.0f;
    for (int i = late_idx; i < FFT_SIZE; i++) late += e_t[i];
    moments[1] = late / total_energy;

    // Dim 28: harmonic absorption ratio (harmonics / fundamental) from grid.
    float fund_e = 0.0f, harm_e = 0.0f;
    for (int t = 0; t < 4; t++) fund_e += grid[t * 4 + 0];
    for (int t = 0; t < 4; t++) { harm_e += grid[t * 4 + 2] + grid[t * 4 + 3]; }
    fund_e += 1e-5f; harm_e += 1e-5f;
    float ratio = (harm_e / fund_e);
    if (ratio > HARMONIC_CLIP) ratio = HARMONIC_CLIP;
    moments[2] = ratio / HARMONIC_CLIP;

    // Dim 29: abbott stiffness proxy (fundamental energy x V^(2/3)).
    // Note: the sampling pipeline overrides moments[3] with raw fund_e below;
    // the abbott normalization (mean fundamental over 4 slices) is applied at
    // that assignment so it stays bit-exact with the Dart compiler.
    float prod = fund_e * volume_norm;
    moments[3] = (prod > 0.0f) ? (prod > 1.0f ? 1.0f : prod) : 0.0f;

    // Dim 30: spectral entropy over mean PSD.
    float psd_sum = 0.0f;
    const int spec_len = STFT_FFT_N / 2 + 1;
    for (int i = 0; i < spec_len; i++) psd_sum += raw_psd_mean[i];
    moments[4] = 0.0f;
    if (psd_sum > 1e-12f) {
        float H = 0.0f;
        for (int i = 0; i < spec_len; i++) {
            float p = raw_psd_mean[i] / psd_sum;
            if (p > 1e-15f) H -= p * logf(p);
        }
        moments[4] = H / logf((float)spec_len);
    }

    // Dim 31: dynamic damping — log-slope of the RMS envelope decay.
    int rms_len = FFT_SIZE - RMS_ENVELOPE_N + 1;
    float* rms_env = dsp_rms_env;
    for (int i = 0; i < rms_len; i++) {
        float acc = 0.0f;
        for (int j = 0; j < RMS_ENVELOPE_N; j++) acc += e_t[i + j];
        rms_env[i] = sqrtf((acc / RMS_ENVELOPE_N) + 1e-7f);
    }
    int peak_rms = 0;
    for (int i = 1; i < rms_len; i++)
        if (rms_env[i] > rms_env[peak_rms]) peak_rms = i;

    int n_valid = 0;
    for (int i = peak_rms; i < rms_len; i++)
        if (rms_env[i] > DECAY_NOISE_GATE) n_valid++;

    if (n_valid > 8) {
        // Collect log energies of the valid points (rms_env no longer needed,
        // reused in-place), then least-squares slope with EVENLY-SPACED x in
        // [0,1] over the valid points ONLY. This matches the validated engine:
        // t_steps = linspace(0, 1, len(y)).
        int cnt = 0;
        for (int i = peak_rms; i < rms_len; i++) {
            if (rms_env[i] <= DECAY_NOISE_GATE) continue;
            rms_env[cnt] = logf(rms_env[i]);
            cnt++;
        }
        float sum_x = 0, sum_y = 0, sum_xx = 0, sum_xy = 0;
        for (int k = 0; k < cnt; k++) {
            float x = (cnt > 1) ? (float)k / (float)(cnt - 1) : 0.0f;
            float y = rms_env[k];
            sum_x += x; sum_y += y; sum_xx += x * x; sum_xy += x * y;
        }
        float slope = 0.0f;
        if (cnt > 1) {
            float denom = cnt * sum_xx - sum_x * sum_x;
            if (fabsf(denom) > 1e-12f)
                slope = (cnt * sum_xy - sum_x * sum_y) / denom;
        }
        // Validated engine negates the least-squares slope (a decaying packet
        // has negative concavity); match that sign convention exactly.
        moments[5] = (-slope) / DAMPING_SCALE;
        if (moments[5] < 0.0f) moments[5] = 0.0f;
        if (moments[5] > 1.0f) moments[5] = 1.0f;
    } else {
        moments[5] = 0.5f;
    }
}

// Background FFT processing execution
void PiezoAcoustic::process_captured_buffer() {
    AcousticFeatures result = {0};
    float max_peak_adc = 0.0f;

    // Per-window mean (bias offset from the charge amp / supply drift).
    // Removed before windowing so it can't leak through the Hanning
    // sidelobes into the low bands and inflate features.
    float mean = 0.0f;
    for (int i = 0; i < FFT_SIZE; i++) {
        if (adc_buffer[i] > max_peak_adc) {
            max_peak_adc = adc_buffer[i];
        }
        mean += adc_buffer[i];
    }
    mean /= FFT_SIZE;

    float max_deviation = 0.0f;
    for (int i = 0; i < FFT_SIZE; i++) {
        const float centered = adc_buffer[i] - mean;
        const float a = centered < 0.0f ? -centered : centered;
        if (a > max_deviation) {
            max_deviation = a;
        }
        fft_input[i * 2] = centered * hanning_window[i];
        fft_input[i * 2 + 1] = 0.0f;
    }

    result.hertzian_adc = max_peak_adc;         // legacy raw peak (CSV parity)
    result.impact_amplitude = max_deviation;    // bias-independent tap strength

#if defined(CONFIG_IDF_TARGET_ESP32S3)
    dsps_fft2r_fc32_ae32(fft_input, FFT_SIZE);
#else
    dsps_fft2r_fc32(fft_input, FFT_SIZE);
#endif

    dsps_bit_rev_fc32(fft_input, FFT_SIZE);

    float bin_resolution = (float)SAMPLING_FREQ_HZ / (float)FFT_SIZE;
    float raw_bin_power[N_FFT_BINS] = {0};

    for (int b = 0; b < N_FFT_BINS; b++) {
        float center_freq = FFT_CENTERS[b];

        int target_fft_bin = (int)((center_freq / bin_resolution) + 0.5f);
        if (target_fft_bin >= (FFT_SIZE / 2)) {
            target_fft_bin = (FFT_SIZE / 2) - 1;
        }

        // Integrate the Hann mainlobe (bin k-1 .. k+1) instead of taking a
        // single bin: tap resonances drift with contact/placement, and a
        // component landing between bins would otherwise lose most of its
        // energy to the neighbors.
        float power = 0.0f;
        for (int kk = target_fft_bin - 1; kk <= target_fft_bin + 1; kk++) {
            if (kk < 0 || kk >= (FFT_SIZE / 2)) continue;
            float real = fft_input[kk * 2];
            float imag = fft_input[kk * 2 + 1];
            power += (real * real) + (imag * imag);
        }

        raw_bin_power[b] = power;

        float conditioned = logf(power + EPS_LOG) * (center_freq * center_freq) / F2_NORM;
        result.fft_bins[b] = (conditioned < FFT_CLAMP_MIN) ? FFT_CLAMP_MIN : conditioned;
    }

    result.spectral_entropy = compute_spectral_entropy(raw_bin_power);

    // ── 32D Fruit Profile: STFT 4x4 image + bio-moments ─────────────
    {
        float* centered_c = dsp_centered;
        for (int i = 0; i < FFT_SIZE; i++) centered_c[i] = adc_buffer[i] - mean;

        // High-pass above ~80 Hz (1st-order, alpha 0.945 @ fs 8820) to strip
        // the ~17 Hz transducer electrical discharge that otherwise dominates
        // the acoustic band. The differentiator term cancels residual DC.
        const float HP_ALPHA = 0.945f;
        float prev = 0.0f;
        for (int i = 1; i < FFT_SIZE; i++) {
            centered_c[i] = HP_ALPHA * (prev + centered_c[i] - centered_c[i-1]);
            prev = centered_c[i];
        }

        float psd_mean[STFT_FFT_N / 2 + 1];
        build_stft_grid(centered_c, result.stft_grid, psd_mean);

        // bio_moments[3] = mean fundamental grid energy (sum over 4 slices
        // normalized by 4) — the assembler scales it by volume^(2/3) to form
        // the abbott stiffness proxy. Normalizing keeps fundamental energy in
        // [0,1] instead of saturating at the 1.0 clamp (raw sum reaches ~4.0).
        float fund_e = 0.0f;
        for (int t = 0; t < 4; t++) fund_e += result.stft_grid[t * 4 + 0];
        compute_bio_moments(centered_c, result.stft_grid, psd_mean, 1.0f,
                            result.bio_moments);
        result.bio_moments[3] = fund_e / 4.0f;
    }

    // Lock mutex and present newly ready features to main application
    if (xSemaphoreTake(data_mutex, portMAX_DELAY)) {
        latest_features = result;
        new_data_available.store(true);
        xSemaphoreGive(data_mutex);
        Serial.printf("[Acoustic] Capture done, peak=%.3f amp=%.3f\n",
                      max_peak_adc, max_deviation);
    }
}
