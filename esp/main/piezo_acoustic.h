#pragma once

#include <Arduino.h>
#include "config.h"

// Background task and synchronization includes
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// All acoustic tunables live in config.h (FFT_SIZE, SAMPLING_FREQ_HZ,
// N_FFT_BINS, F2_NORM, EPS_LOG, FFT_CLAMP_MIN, PRE_TRIGGER_SAMPLES,
// RING_BUFFER_SIZE, PIEZO_FLUSH_SAMPLES, TRIGGER_DELTA_MIN, gate params...).

struct AcousticFeatures {
    float hertzian_adc;       // Peak raw level in window (0-1, legacy/CSV parity)
    float impact_amplitude;   // Peak |sample - window mean| (0-1) — true tap strength
    float fft_bins[N_FFT_BINS]; //15 f^2 - conditioned log power bins
    float spectral_entropy; //Normalized s.e.
    // ── 32D Fruit Profile additions ───────────────────────────────
    float stft_grid[16];      // 4x4 time-frequency image (log1p band power),
                              // contrast-normalized to 0..1 in [10..25]
    float bio_moments[6];     // dim 26..31: temporal centroid, tail energy,
                              // harmonic absorption, abbott stiffness,
                              // spectral entropy, dynamic damping
};

enum SamplingState {
    STATE_DISARMED,
    STATE_ARMED,        // Continuously cachin
    STATE_CAPTURING,    // Threshold hit
    STATE_PROCESSING    // Data locked
};

class PiezoAcoustic {
    private:
        adc1_channel_t adc_channel;

        //16-bytes for SIMD
        alignas(16) float adc_buffer[FFT_SIZE];
        alignas(16) float fft_input[FFT_SIZE * 2]; //real and imaginary
        alignas(16) float hanning_window[FFT_SIZE];

        //Center frequencies for the 15 bands (hz)
        const float FFT_CENTERS[N_FFT_BINS] = {
            150.0f, 250.0f, 350.0f, 450.0f, 550.0f,
            650.0f, 750.0f, 850.0f, 950.0f, 1100.0f,
            1300.0f, 1500.0f, 1700.0f, 1900.0f, 2100.0f
        };

        void generate_hanning_window();
        float compute_spectral_entropy(const float raw_power[N_FFT_BINS]);

        // 32D Fruit Profile DSP: STFT 4x4 grid + 6 bio-moments, computed
        // from the full centered window during process_captured_buffer().
        alignas(16) float stft_fft[STFT_FFT_N * 2]; // per-frame FFT workspace
        alignas(16) float stft_win[STFT_FFT_N];     // Hanning for n_fft=128
        // Large working buffers kept as members (NOT stack) so the 4KB
        // sampler task never overflows: e_t/rms_env/centered are 512 floats.
        alignas(16) float dsp_centered[FFT_SIZE];
        alignas(16) float dsp_energy[FFT_SIZE];
        alignas(16) float dsp_rms_env[FFT_SIZE];
        void build_stft_grid(const float centered[FFT_SIZE],
                             float out[16],
                             float raw_psd_mean[STFT_FFT_N / 2 + 1]);
        void compute_bio_moments(const float centered[FFT_SIZE],
                                 const float grid[16],
                                 const float raw_psd_mean[STFT_FFT_N / 2 + 1],
                                 float volume_norm,
                                 float moments[6]);

        // Continuous Background Recording Extensions
        alignas(16) float ring_buffer[RING_BUFFER_SIZE];
        volatile uint16_t ring_head = 0;

        std::atomic<SamplingState> state{STATE_DISARMED};
        float trigger_threshold_adc = 0.15f; // min |x - baseline| (deviation, 0-1)
        uint32_t settle_us = 0;      // ignore triggers this long after arm()
        uint16_t post_trigger_counter = 0;
        uint32_t armed_start_us = 0;

        // Slow ambient baseline for deviation-based triggering. Locked to the
        // first sample of each session, then EMA-tracked: fast during settle
        // (absorbs LCD/button burst ring), slow while listening.
        float baseline = 0.0f;

        // Derivative trigger: detect sharp impulse (high delta) even when
        // absolute value is below threshold. Fruit taps produce brief spikes
        // (<200µs) that cross threshold for <2 samples but have high slope.
        // delta >= 0.015 catches the impulse; the baseline-deviation check
        // catches strong taps and provides a fallback.
        float prev_sample_val = 0.0f;
        float slew_noise_ema = SLEW_NOISE_EMA_INIT;
        float dev_noise_ema = DEV_NOISE_EMA_INIT;
        uint16_t samples_since_arm = 0;

        TaskHandle_t sampler_task_handle = nullptr;
        SemaphoreHandle_t data_mutex = nullptr;

        AcousticFeatures latest_features = {0};
        std::atomic<bool> new_data_available{false};

        void process_captured_buffer();
        static void sampler_task_code(void* parameter);

    public:
        PiezoAcoustic(adc1_channel_t channel = ADC1_CHANNEL_6);

        bool init();
        void capture_raw_waveform(uint16_t raw_out[512], uint16_t* peak_out);

        // Expose the background sampler's captured 512-sample window (64
        // pre-trigger + 448 post-trigger) as raw ADC values. Call after
        // is_data_ready() returns true. Clears the data-ready flag.
        bool capture_sampler_raw_window(uint16_t raw_out[512], uint16_t* peak_out);

        // Continuous Background Recording API
        // trigger_threshold_adc is a DEVIATION threshold: |sample - baseline|
        // (normalized 0-1), not a raw ADC level. settle_us keeps the trigger
        // masked while post-arm electrical/mechanical ringing decays; the
        // baseline re-tracks quickly during that window.
        void arm(float trigger_threshold = 0.02f, uint32_t settle_us = 0);
        void disarm();
        bool is_data_ready();
        AcousticFeatures get_latest_features();
        SamplingState get_state();
};
