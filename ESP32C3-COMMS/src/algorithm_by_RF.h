#ifndef ALGORITHM_BY_RF_H_
#define ALGORITHM_BY_RF_H_
#include <Arduino.h>

/*
 * Settable parameters 
 * Leave these alone if your circuit and hardware setup match the defaults 
 * described in this code's Instructable. Typically, different sampling rate
 * and/or sample length would require these paramteres to be adjusted.
 */
#define ST 4      // Sampling time in s. WARNING: if you change ST, then you MUST recalcuate the sum_X2 parameter below!
#define FS 25     // Sampling frequency in Hz. WARNING: if you change FS, then you MUST recalcuate the sum_X2 parameter below!
const float sum_X2 = 83325; // WARNING: you MUST recalculate this sum if you changed either ST or FS above!
#define MAX_HR 180  // Maximal heart rate. To eliminate erroneous signals, calculated HR should never be greater than this number.
#define MIN_HR 40   // Minimal heart rate. To eliminate erroneous signals, calculated HR should never be lower than this number.

// [FIX] Naikkan dari 0.35 -> 0.5. Threshold ini menentukan seberapa "tegas"
// sebuah peak autocorrelation harus dibanding lag=0 sebelum dianggap periode
// detak yang valid. Nilai 0.35 (default RF) terlalu longgar untuk sensor ini,
// sehingga kadang algoritma "lock" ke harmonik tetangga yang salah (BPM
// bolak-balik antar dua nilai berdekatan, mis. 93 <-> 107). Menaikkan ke 0.5
// membuat algoritma lebih pemilih: hanya menerima periodisitas yang benar-benar
// dominan, sehingga hasil lebih konsisten dengan referensi (smartwatch).
// Trade-off: kalau kontak jari/sinyal kurang bagus, kemungkinan lebih sering
// muncul "Sinyal tidak stabil" (hr_valid=0) -- itu wajar, lebih baik diam
// daripada kasih angka yang salah.
const float min_autocorrelation_ratio = 0.5;

// Pearson correlation between red and IR signals.
// Good quality signals must have their correlation coefficient greater than this minimum.
const float min_pearson_correlation = 0.7;

/*
 * Derived parameters 
 * Do not touch these! 
 */
const int32_t BUFFER_SIZE = FS*ST; // Number of smaples in a single batch
const int32_t FS60 = FS*60;  // Conversion factor for heart rate from bps to bpm
const int32_t LOWEST_PERIOD = FS60/MAX_HR; // Minimal distance between peaks
const int32_t HIGHEST_PERIOD = FS60/MIN_HR; // Maximal distance between peaks
const float mean_X = (float)(BUFFER_SIZE-1)/2.0; // Mean value of the set of integers from 0 to BUFFER_SIZE-1.

// [FIX] Parameter terakhir: pn_heart_rate_precise — BPM hasil interpolasi
// parabolik (sub-sample), lebih presisi daripada pn_heart_rate yang integer.
void rf_heart_rate_and_oxygen_saturation(uint32_t *pun_ir_buffer, int32_t n_ir_buffer_length, uint32_t *pun_red_buffer, float *pn_spo2, int8_t *pch_spo2_valid, int32_t *pn_heart_rate, 
                                        int8_t *pch_hr_valid, float *ratio, float *correl, float *pn_heart_rate_precise);
float rf_linear_regression_beta(float *pn_x, float xmean, float sum_x2);
float rf_autocorrelation(float *pn_x, int32_t n_size, int32_t n_lag);
float rf_rms(float *pn_x, int32_t n_size, float *sumsq);
float rf_Pcorrelation(float *pn_x, float *pn_y, int32_t n_size);
void rf_initialize_periodicity_search(float *pn_x, int32_t n_size, int32_t *p_last_periodicity, int32_t n_max_distance, float min_aut_ratio, float aut_lag0);
void rf_signal_periodicity(float *pn_x, int32_t n_size, int32_t *p_last_periodicity, int32_t n_min_distance, int32_t n_max_distance, float min_aut_ratio, float aut_lag0, float *ratio);

#endif /* ALGORITHM_BY_RF_H_ */