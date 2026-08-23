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
        4096,
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

    // Lock mutex and present newly ready features to main application
    if (xSemaphoreTake(data_mutex, portMAX_DELAY)) {
        latest_features = result;
        new_data_available.store(true);
        xSemaphoreGive(data_mutex);
        Serial.printf("[Acoustic] Capture done, peak=%.3f amp=%.3f\n",
                      max_peak_adc, max_deviation);
    }
}
