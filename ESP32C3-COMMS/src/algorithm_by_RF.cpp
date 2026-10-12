#include "algorithm_by_RF.h"
#include <math.h>

void rf_heart_rate_and_oxygen_saturation(uint32_t *pun_ir_buffer, int32_t n_ir_buffer_length, uint32_t *pun_red_buffer, float *pn_spo2, int8_t *pch_spo2_valid, 
                int32_t *pn_heart_rate, int8_t *pch_hr_valid, float *ratio, float *correl, float *pn_heart_rate_precise)
{
  int32_t k;  
  static int32_t n_last_peak_interval=LOWEST_PERIOD;
  float f_ir_mean,f_red_mean,f_ir_sumsq,f_red_sumsq;
  float f_y_ac, f_x_ac, xy_ratio;
  float beta_ir, beta_red, x;
  float an_x[BUFFER_SIZE], *ptr_x;
  float an_y[BUFFER_SIZE], *ptr_y;

  f_ir_mean=0.0; 
  f_red_mean=0.0;
  for (k=0; k<n_ir_buffer_length; ++k) {
    f_ir_mean += pun_ir_buffer[k];
    f_red_mean += pun_red_buffer[k];
  }
  f_ir_mean=f_ir_mean/n_ir_buffer_length ;
  f_red_mean=f_red_mean/n_ir_buffer_length ;
  
  for (k=0,ptr_x=an_x,ptr_y=an_y; k<n_ir_buffer_length; ++k,++ptr_x,++ptr_y) {
    *ptr_x = pun_ir_buffer[k] - f_ir_mean;
    *ptr_y = pun_red_buffer[k] - f_red_mean;
  }

  beta_ir = rf_linear_regression_beta(an_x, mean_X, sum_X2);
  beta_red = rf_linear_regression_beta(an_y, mean_X, sum_X2);
  for(k=0,x=-mean_X,ptr_x=an_x,ptr_y=an_y; k<n_ir_buffer_length; ++k,++x,++ptr_x,++ptr_y) {
    *ptr_x -= beta_ir*x;
    *ptr_y -= beta_red*x;
  }
  
  f_y_ac=rf_rms(an_y,n_ir_buffer_length,&f_red_sumsq);
  f_x_ac=rf_rms(an_x,n_ir_buffer_length,&f_ir_sumsq);

  *correl=rf_Pcorrelation(an_x, an_y, n_ir_buffer_length)/sqrt(f_red_sumsq*f_ir_sumsq);

  if(*correl>=min_pearson_correlation) {
    if(LOWEST_PERIOD==n_last_peak_interval) 
      rf_initialize_periodicity_search(an_x, BUFFER_SIZE, &n_last_peak_interval, HIGHEST_PERIOD, min_autocorrelation_ratio, f_ir_sumsq);
    if(n_last_peak_interval!=0)
      rf_signal_periodicity(an_x, BUFFER_SIZE, &n_last_peak_interval, LOWEST_PERIOD, HIGHEST_PERIOD, min_autocorrelation_ratio, f_ir_sumsq, ratio);
  } else n_last_peak_interval=0;

  if(n_last_peak_interval!=0) {
    *pn_heart_rate = (int32_t)(FS60/n_last_peak_interval);
    *pch_hr_valid  = 1;

    // [FIX] Interpolasi parabolik di sekitar lag terbaik untuk presisi sub-sample.
    float refined_lag = (float)n_last_peak_interval;
    if (n_last_peak_interval - 1 >= LOWEST_PERIOD && n_last_peak_interval + 1 <= HIGHEST_PERIOD) {
      float aut_m1 = rf_autocorrelation(an_x, n_ir_buffer_length, n_last_peak_interval - 1);
      float aut_0  = rf_autocorrelation(an_x, n_ir_buffer_length, n_last_peak_interval);
      float aut_p1 = rf_autocorrelation(an_x, n_ir_buffer_length, n_last_peak_interval + 1);
      float denom  = (aut_m1 - 2.0f * aut_0 + aut_p1);
      if (fabs(denom) > 1e-6f) {
        float delta = 0.5f * (aut_m1 - aut_p1) / denom;
        if (delta > -1.0f && delta < 1.0f) {
          refined_lag = (float)n_last_peak_interval + delta;
        }
      }
    }
    *pn_heart_rate_precise = (float)FS60 / refined_lag;
  } else {
    n_last_peak_interval=LOWEST_PERIOD;
    *pn_heart_rate = -999;
    *pch_hr_valid  = 0;
    *pn_spo2 =  -999 ;
    *pch_spo2_valid  = 0; 
    *pn_heart_rate_precise = -999.0f;
    return;
  }

  xy_ratio= (f_y_ac*f_ir_mean)/(f_x_ac*f_red_mean);
  if(xy_ratio>0.02 && xy_ratio<1.84) {
    *pn_spo2 = (-45.060*xy_ratio + 30.354)*xy_ratio + 94.845;
    *pch_spo2_valid = 1;
  } else {
    *pn_spo2 =  -999 ;
    *pch_spo2_valid  = 0; 
  }
}

float rf_linear_regression_beta(float *pn_x, float xmean, float sum_x2)
{
  float x,beta,*pn_ptr;
  beta=0.0;
  for(x=-xmean,pn_ptr=pn_x;x<=xmean;++x,++pn_ptr)
    beta+=x*(*pn_ptr);
  return beta/sum_x2;
}

float rf_autocorrelation(float *pn_x, int32_t n_size, int32_t n_lag) 
{
  int16_t i, n_temp=n_size-n_lag;
  float sum=0.0,*pn_ptr;
  if(n_temp<=0) return sum;
  for (i=0,pn_ptr=pn_x; i<n_temp; ++i,++pn_ptr) {
    sum += (*pn_ptr)*(*(pn_ptr+n_lag));
  }
  return sum/n_temp;
}

void rf_initialize_periodicity_search(float *pn_x, int32_t n_size, int32_t *p_last_periodicity, int32_t n_max_distance, float min_aut_ratio, float aut_lag0)
{
  int32_t n_lag;
  float aut,aut_right;
  n_lag=*p_last_periodicity;
  aut_right=aut=rf_autocorrelation(pn_x, n_size, n_lag);
  if(aut/aut_lag0 >= min_aut_ratio) {
    do {
      aut=aut_right;
      n_lag+=2;
      aut_right=rf_autocorrelation(pn_x, n_size, n_lag);
    } while(aut_right/aut_lag0 >= min_aut_ratio && aut_right<aut && n_lag<=n_max_distance);
    if(n_lag>n_max_distance) {
      *p_last_periodicity=0;
      return;
    }
    aut=aut_right;
  }
  do {
    aut=aut_right;
    n_lag+=2;
    aut_right=rf_autocorrelation(pn_x, n_size, n_lag);
  } while(aut_right/aut_lag0 < min_aut_ratio && n_lag<=n_max_distance);
  if(n_lag>n_max_distance) {
    *p_last_periodicity=0;
  } else
    *p_last_periodicity=n_lag;
}

void rf_signal_periodicity(float *pn_x, int32_t n_size, int32_t *p_last_periodicity, int32_t n_min_distance, int32_t n_max_distance, float min_aut_ratio, float aut_lag0, float *ratio)
{
  int32_t n_lag;
  float aut,aut_left,aut_right,aut_save;
  bool left_limit_reached=false;
  n_lag=*p_last_periodicity;
  aut_save=aut=rf_autocorrelation(pn_x, n_size, n_lag);
  aut_left=aut;
  do {
    aut=aut_left;
    n_lag--;
    aut_left=rf_autocorrelation(pn_x, n_size, n_lag);
  } while(aut_left>aut && n_lag>=n_min_distance);
  if(n_lag<n_min_distance) {
    left_limit_reached=true;
    n_lag=*p_last_periodicity;
    aut=aut_save;
  } else n_lag++;
  if(n_lag==*p_last_periodicity) {
    aut_right=aut;
    do {
      aut=aut_right;
      n_lag++;
      aut_right=rf_autocorrelation(pn_x, n_size, n_lag);
    } while(aut_right>aut && n_lag<=n_max_distance);
    if(n_lag>n_max_distance) n_lag=0;
    else n_lag--;
    if(n_lag==*p_last_periodicity && left_limit_reached) n_lag=0;
  }
  *ratio=aut/aut_lag0;
  if(*ratio < min_aut_ratio) n_lag=0;
  *p_last_periodicity=n_lag;
}

float rf_rms(float *pn_x, int32_t n_size, float *sumsq) 
{
  int16_t i;
  float r,*pn_ptr;
  (*sumsq)=0.0;
  for (i=0,pn_ptr=pn_x; i<n_size; ++i,++pn_ptr) {
    r=(*pn_ptr);
    (*sumsq) += r*r;
  }
  (*sumsq)/=n_size;
  return sqrt(*sumsq);
}

float rf_Pcorrelation(float *pn_x, float *pn_y, int32_t n_size)
{
  int16_t i;
  float r,*x_ptr,*y_ptr;
  r=0.0;
  for (i=0,x_ptr=pn_x,y_ptr=pn_y; i<n_size; ++i,++x_ptr,++y_ptr) {
    r+=(*x_ptr)*(*y_ptr);
  }
  r/=n_size;
  return r;
}