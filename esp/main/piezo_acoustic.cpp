#include "piezo_acoustic.h"
#include "driver/adc.h"
#include "dsps_fft2r.h"
#include "hal/adc_types.h"
#include "sdkconfig.h"
#include <cstdint>
#include <esp32-hal-adc.h>

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

    Serial.println("[Piezo] Hardware FFT Subsystem Initialized.");
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
