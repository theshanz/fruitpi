#pragma once

#include <Arduino.h>
#include <driver/adc.h> // rplacing analogRead

// Background task and synchronization includes
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define FFT_SIZE 512 //Must be 2^N
#define SAMPLING_FREQ_HZ 8820 //Nyquist = 4410 Hz
#define N_FFT_BINS 15 //15 28-D vector slots
#define F2_NORM 441000.0F //Normalization
#define EPS_LOG 1e-10f
#define FFT_CLAMP_MIN -10.0f
#define PRE_TRIGGER_SAMPLES 64
#define RING_BUFFER_SIZE 1024 // Double FFT_SIZE for ring cache

struct AcousticFeatures {
    float hertzian_adc; //Peak impact voltage
    float fft_bins[N_FFT_BINS]; //15 f^2 - conditioned log power bins
    float spectral_entropy; //Normalized s.e.
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

        // Continuous Background Recording Extensions
        alignas(16) float ring_buffer[RING_BUFFER_SIZE];
        volatile uint16_t ring_head = 0;

        std::atomic<SamplingState> state{STATE_DISARMED};
        float trigger_threshold_adc = 0.15f;
        uint16_t post_trigger_counter = 0;

        TaskHandle_t sampler_task_handle = nullptr;
        SemaphoreHandle_t data_mutex = nullptr;

        AcousticFeatures latest_features = {0};
        std::atomic<bool> new_data_available{false};

        void process_captured_buffer();
        static void sampler_task_code(void* parameter);

    public:
        PiezoAcoustic(adc1_channel_t channel = ADC1_CHANNEL_6);

        bool init();
        bool check_impact_detected(float threshold_adc = 0.15f);
        AcousticFeatures capture_and_process();

        void capture_raw_waveform(uint16_t raw_out[512], uint16_t* peak_out);

        bool start_arming();

        // Continuous Background Recording API
        void arm(float trigger_threshold = 0.15f);
        void disarm();
        bool is_data_ready();
        AcousticFeatures get_latest_features();
        SamplingState get_state();
};
