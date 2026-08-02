#include "piezo_acoustic.h"
#include "driver/adc.h"
#include "dsps_fft2r.h"
#include "hal/adc_types.h"
#include "sdkconfig.h"
#include <cstdint>
#include <cstring>
#include <esp32-hal-adc.h>
#include <pgmspace.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

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

    // Spawn background continuous sampling task on Core 0
    xTaskCreatePinnedToCore(
        PiezoAcoustic::sampler_task_code,
        "PiezoSamplerTask",
        4096,
        this,
        10,                      // High priority
        &sampler_task_handle,
        0                        // Core 0
    );

    Serial.println("[Piezo] Hardware FFT & Background Sampler Subsystem Initialized.");
    return true;
}

bool PiezoAcoustic::check_impact_detected(float threshold_adc){
    int raw = adc1_get_raw(adc_channel);
    float normalized = (float)raw / 4095.0f;
    return (normalized > threshold_adc);
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

AcousticFeatures PiezoAcoustic::capture_and_process(){
    AcousticFeatures result = {0};

    float max_peak_adc = 0.0f;
    uint8_t sample_period_us = 1000000 / SAMPLING_FREQ_HZ; // ~133

    for (int i = 0; i < FFT_SIZE; i++ ){
        uint32_t t_start = micros();

        int raw_adc = adc1_get_raw(adc_channel);
        float normalized_val = (float)raw_adc / 4095.0f;

        if (normalized_val > max_peak_adc){
            max_peak_adc = normalized_val;
        }

        adc_buffer[i] = normalized_val;

        while ((micros() - t_start) < sample_period_us);

    }

    result.hertzian_adc = max_peak_adc;

    for (int i = 0; i < FFT_SIZE; i++) {
        fft_input[i * 2] = adc_buffer[i] * hanning_window [i]; //real (Windowed)
        fft_input[i * 2 + 1] = 0.0f; //imginary
    }
    #if defined(CONFIG_IDF_TARGET_ESP32S3)
        dsps_fft2r_fc32_ae32(fft_input, FFT_SIZE); // ESP32-S3 SIMD
    #else
        dsps_fft2r_fc32(fft_input, FFT_SIZE);      // Generic ESP32 fallback
    #endif

    dsps_bit_rev_fc32(fft_input, FFT_SIZE);

    float bin_resolution = (float)SAMPLING_FREQ_HZ / (float)FFT_SIZE;
    float raw_bin_power[N_FFT_BINS] = {0};

    for (int b = 0; b < N_FFT_BINS; b++){
        float center_freq = FFT_CENTERS[b];

        //find nearest FFT bin intex
        int target_fft_bin = (int)((center_freq / bin_resolution) + 0.5f);
        if(target_fft_bin >= (FFT_SIZE / 2)){
            target_fft_bin = (FFT_SIZE / 2) - 1;
        }

        //extract real and img components
        float real = fft_input[target_fft_bin * 2];
        float imag = fft_input[target_fft_bin * 2 + 1];
        float power = (real * real) + (imag * imag);

        raw_bin_power[b] = power;

        float conditioned = logf(power +  EPS_LOG) * (center_freq * center_freq) / F2_NORM;

        result.fft_bins[b] = (conditioned < FFT_CLAMP_MIN) ? FFT_CLAMP_MIN : conditioned;

    }
    result.spectral_entropy = compute_spectral_entropy(raw_bin_power);
    return result;

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

//////////////////Continuous Background Recording Subsystem Methods /////////////////////////////////////

bool PiezoAcoustic::start_arming() {
    arm(0.15f);
    return true;
}

void PiezoAcoustic::arm(float trigger_threshold) {
    if (xSemaphoreTake(data_mutex, portMAX_DELAY)) {
        trigger_threshold_adc = trigger_threshold;
        new_data_available.store(false);
        post_trigger_counter = 0;

        //prevent old data on new arm sequence
        memset(ring_buffer, 0, sizeof(ring_buffer));
        ring_head = 0;

        state.store(STATE_ARMED);
        xSemaphoreGive(data_mutex);
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

// FreeRTOS Task: High-priority background loop on Core 0
void PiezoAcoustic::sampler_task_code(void* parameter) {
    PiezoAcoustic* self = static_cast<PiezoAcoustic*>(parameter);
    uint32_t sample_period_us = 1000000 / SAMPLING_FREQ_HZ; // ~113 us

    while (true) {
        uint32_t t_start = micros();

        // 1. Always continuously sample into circular ring buffer
        int raw_adc = adc1_get_raw(self->adc_channel);
        float normalized_val = (float)raw_adc / 4095.0f;

        uint16_t current_head = self->ring_head;
        self->ring_buffer[current_head] = normalized_val;
        self->ring_head = (current_head + 1) % RING_BUFFER_SIZE;

        // 2. State machine evaluation
        SamplingState cur_state = self->state.load();

        if (cur_state == STATE_ARMED) {
            // Check for trigger impact threshold crossing
            if (normalized_val >= self->trigger_threshold_adc) {
                self->post_trigger_counter = 0;
                self->state.store(STATE_CAPTURING);
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
    }
}

// Background FFT processing execution
void PiezoAcoustic::process_captured_buffer() {
    AcousticFeatures result = {0};
    float max_peak_adc = 0.0f;

    for (int i = 0; i < FFT_SIZE; i++) {
        if (adc_buffer[i] > max_peak_adc) {
            max_peak_adc = adc_buffer[i];
        }
        fft_input[i * 2] = adc_buffer[i] * hanning_window[i];
        fft_input[i * 2 + 1] = 0.0f;
    }

    result.hertzian_adc = max_peak_adc;

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

        float real = fft_input[target_fft_bin * 2];
        float imag = fft_input[target_fft_bin * 2 + 1];
        float power = (real * real) + (imag * imag);

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
    }
}
