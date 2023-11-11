

#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"

#if CONFIG_USE_DSP_PROCESSOR
#include "dsp_processor.h"
#include "dsps_biquad.h"
#include "dsps_biquad_gen.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifdef CONFIG_USE_BIQUAD_ASM
#define BIQUAD dsps_biquad_f32_ae32
#else
#define BIQUAD dsps_biquad_f32
#endif

static const char *TAG = "dspProc";

#define DSP_PROCESSOR_LEN 16

static QueueHandle_t filterUpdateQHdl = NULL;

static filterParams_t filterParams;

static ptype_t *filter = NULL;

static double dynamic_vol = 1.0;

static bool init = false;

static float *sbuffer0 = NULL;
static float *sbufout0 = NULL;

#if CONFIG_USE_DSP_PROCESSOR
#if CONFIG_SNAPCLIENT_DSP_FLOW_STEREO
dspFlows_t dspFlowInit = dspfStereo;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BASSBOOST
dspFlows_t dspFlowInit = dspfBassBoost;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BIAMP
dspFlows_t dspFlowInit = dspfBiamp;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BASS_TREBLE_EQ
dspFlows_t dspFlowInit = dspfEQBassTreble;
#endif
#endif

/*
 *
 */
esp_err_t nvs_init(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  return err;
}

void nvs_get_or_default(filterParams_t *params) {
  nvs_handle_t nvs_handler;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handler);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Can't open NVS partition, load default value.");
    return;
  } else {
    int16_t param_value;

    err = nvs_get_i16(nvs_handler, "dspFlow", &param_value);
    if (err != ESP_OK) return;
    if (param_value != params->dspFlow) {
      ESP_LOGI(TAG,
               "Saved DSP FLOW different from config. Loading default value.");
      return;
    }
    params->dspFlow = param_value;

    err = nvs_get_i16(nvs_handler, "fc_1", &param_value);
    if (err != ESP_OK) return;
    params->fc_1 = (float)(param_value);

    err = nvs_get_i16(nvs_handler, "fc_2", &param_value);
    if (err != ESP_OK) return;
    params->fc_2 = (float)(param_value);

    err = nvs_get_i16(nvs_handler, "fc_3", &param_value);
    if (err != ESP_OK) return;
    params->fc_3 = (float)(param_value);

    err = nvs_get_i16(nvs_handler, "gain_1", &param_value);
    if (err != ESP_OK) return;
    params->gain_1 = (float)(param_value) / 100;

    err = nvs_get_i16(nvs_handler, "gain_2", &param_value);
    if (err != ESP_OK) return;
    params->gain_2 = (float)(param_value) / 100;

    err = nvs_get_i16(nvs_handler, "gain_3", &param_value);
    if (err != ESP_OK) return;
    params->gain_3 = (float)(param_value) / 100;
  }
  ESP_LOGD(TAG, "Loaded filter params from NVS.");
  return;
}

void nvs_save(filterParams_t params) {
  nvs_handle_t nvs_handler;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handler);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Unable to save DSP params. Can't open NVS partition.");
    return;
  } else {
    err = nvs_set_i16(nvs_handler, "dspFlow", params.dspFlow);
    if (err != ESP_OK) return;

    err = nvs_set_i16(nvs_handler, "fc_1", (int16_t)(params.fc_1));
    if (err != ESP_OK) return;

    err = nvs_set_i16(nvs_handler, "fc_2", (int16_t)(params.fc_2));
    if (err != ESP_OK) return;

    err = nvs_set_i16(nvs_handler, "fc_3", (int16_t)(params.fc_3));
    if (err != ESP_OK) return;

    err = nvs_set_i16(nvs_handler, "gain_1", (int16_t)(params.gain_1 * 100));
    if (err != ESP_OK) return;

    err = nvs_set_i16(nvs_handler, "gain_2", (int16_t)(params.gain_2 * 100));
    if (err != ESP_OK) return;

    err = nvs_set_i16(nvs_handler, "gain_3", (int16_t)(params.gain_3 * 100));
    if (err != ESP_OK) return;
  }
  ESP_LOGD(TAG, "Saved filter params from NVS.");
  return;
}

/**
 *
 */
void dsp_processor_init(void) {
  init = false;

  if (filterUpdateQHdl) {
    vQueueDelete(filterUpdateQHdl);
    filterUpdateQHdl = NULL;
  }

  // have a max queue length of 1 here because we use xQueueOverwrite
  // to write to the queue
  filterUpdateQHdl = xQueueCreate(1, sizeof(filterParams_t));
  if (filterUpdateQHdl == NULL) {
    ESP_LOGE(TAG, "%s: Failed to create filter update queue", __func__);
    return;
  }

  filterParams.dspFlow = dspFlowInit;

  switch (filterParams.dspFlow) {
    case dspfEQBassTreble: {
      filterParams.fc_1 = 300.0;
      filterParams.gain_1 = 0.0;
      filterParams.fc_2 = 1000.0;
      filterParams.gain_2 = 0.0;
      filterParams.fc_3 = 4000.0;
      filterParams.gain_3 = 0.0;

      break;
    }

    case dspfStereo: {
      break;
    }

    case dspfBassBoost: {
      filterParams.fc_1 = 300.0;
      filterParams.gain_1 = 6.0;
      break;
    }

    case dspfBiamp: {
      filterParams.fc_1 = 300.0;
      filterParams.gain_1 = 0;
      filterParams.fc_3 = 100.0;
      filterParams.gain_3 = 0.0;
      break;
    }

    case dspf2DOT1: {  // Process audio L + R LOW PASS FILTER
      ESP_LOGW(TAG, "dspf2DOT1, not implemented yet, using stereo instead");
    } break;

    case dspfFunkyHonda: {  // Process audio L + R LOW PASS FILTER
      ESP_LOGW(TAG,
               "dspfFunkyHonda, not implemented yet, using stereo instead");
      break;
    }

    default: {
      break;
    }
  }

  nvs_get_or_default(&filterParams);
  dsp_processor_log_filter(filterParams);

  ESP_LOGI(TAG, "%s: init done", __func__);
}

/**
 * free previously allocated memories
 */
void dsp_processor_uninit(void) {
  if (sbuffer0) {
    free(sbuffer0);
    sbuffer0 = NULL;
  }

  if (sbufout0) {
    free(sbufout0);
    sbufout0 = NULL;
  }

  if (filter) {
    free(filter);
    filter = NULL;
  }

  if (filterUpdateQHdl) {
    vQueueDelete(filterUpdateQHdl);
    filterUpdateQHdl = NULL;
  }

  init = false;

  ESP_LOGI(TAG, "%s: uninit done", __func__);
}

/**
 *
 */
esp_err_t dsp_processor_update_filter_params(filterParams_t *params) {
  if (filterUpdateQHdl) {
    if (xQueueOverwrite(filterUpdateQHdl, params) == pdTRUE) {
      return ESP_OK;
    }
  }

  return ESP_FAIL;
}

/**
 *
 */
static int32_t dsp_processor_gen_filter(ptype_t *filter, uint32_t cnt) {
  if ((filter == NULL) && (cnt > 0)) {
    return ESP_FAIL;
  }

  for (int n = 0; n < cnt; n++) {
    switch (filter[n].filtertype) {
      case HIGHSHELF:
        dsps_biquad_gen_highShelf_f32(filter[n].coeffs, filter[n].freq,
                                      filter[n].gain, filter[n].q);
        break;

      case LOWSHELF:
        dsps_biquad_gen_lowShelf_f32(filter[n].coeffs, filter[n].freq,
                                     filter[n].gain, filter[n].q);
        break;

      case LPF:
        dsps_biquad_gen_lpf_f32(filter[n].coeffs, filter[n].freq, filter[n].q);
        break;

      case HPF:
        dsps_biquad_gen_hpf_f32(filter[n].coeffs, filter[n].freq, filter[n].q);
        break;
      case NOTCH:
        dsps_biquad_gen_notch_f32(filter[n].coeffs, filter[n].freq,
                                  filter[n].gain, filter[n].q);
        break;

      default:
        break;
    }
    //    for (uint8_t i = 0; i <= 4; i++) {
    //      printf("%.6f ", filter[n].coeffs[i]);
    //    }
    //    printf("\n");
  }

  return ESP_OK;
}

/**
 *
 */
int dsp_processor_worker(char *audio, size_t chunk_size, uint32_t samplerate) {
  int16_t len = chunk_size / 4;
  int16_t valint;
  uint16_t i;
  // volatile needed to ensure 32 bit access
  volatile uint32_t *audio_tmp = (volatile uint32_t *)audio;
  dspFlows_t dspFlow;

  // check if we need to update filters
  if (xQueueReceive(filterUpdateQHdl, &filterParams, pdMS_TO_TICKS(0)) ==
      pdTRUE) {
    init = false;

    nvs_save(filterParams);
    dsp_processor_log_filter(filterParams);
  }

  dspFlow = filterParams.dspFlow;

  if (init == false) {
    uint32_t cnt = 0;

    if (filter) {
      free(filter);
      filter = NULL;
    }

    switch (dspFlow) {
      case dspfEQBassTreble: {
        cnt = 6;  // 3-band stereo

        filter =
            (ptype_t *)heap_caps_malloc(sizeof(ptype_t) * cnt, MALLOC_CAP_8BIT);
        if (filter) {
          // simple EQ control of low and high frequencies (bass, treble)
          float bass_fc = filterParams.fc_1 / samplerate;
          float bass_gain = filterParams.gain_1;
          float mids_fc = filterParams.fc_2 / samplerate;
          float mids_gain = filterParams.gain_2;
          float treble_fc = filterParams.fc_3 / samplerate;
          float treble_gain = filterParams.gain_3;

          // filters for CH 0
          filter[0] = (ptype_t){LOWSHELF, bass_fc, bass_gain,       0.707,
                                NULL,     NULL,    {0, 0, 0, 0, 0}, {0, 0}};
          filter[1] = (ptype_t){NOTCH, mids_fc, mids_gain,       0.707,
                                NULL,  NULL,    {0, 0, 0, 0, 0}, {0, 0}};
          filter[2] = (ptype_t){HIGHSHELF, treble_fc, treble_gain,     0.707,
                                NULL,      NULL,      {0, 0, 0, 0, 0}, {0, 0}};
          // filters for CH 1
          filter[3] = (ptype_t){LOWSHELF, bass_fc, bass_gain,       0.707,
                                NULL,     NULL,    {0, 0, 0, 0, 0}, {0, 0}};
          filter[4] = (ptype_t){NOTCH, mids_fc, mids_gain,       0.707,
                                NULL,  NULL,    {0, 0, 0, 0, 0}, {0, 0}};
          filter[5] = (ptype_t){HIGHSHELF, treble_fc, treble_gain,     0.707,
                                NULL,      NULL,      {0, 0, 0, 0, 0}, {0, 0}};

          ESP_LOGI(TAG, "got new setting for dspfEQBassTreble");
        } else {
          ESP_LOGE(TAG, "failed to get memory for filter");
        }

        break;
      }

      case dspfStereo: {
        cnt = 0;
        break;
      }

      case dspfBassBoost: {
        cnt = 2;

        filter =
            (ptype_t *)heap_caps_malloc(sizeof(ptype_t) * cnt, MALLOC_CAP_8BIT);
        if (filter) {
          float bass_fc = filterParams.fc_1 / samplerate;
          float bass_gain = 6.0;

          filter[0] = (ptype_t){LOWSHELF, bass_fc, bass_gain,       0.707,
                                NULL,     NULL,    {0, 0, 0, 0, 0}, {0, 0}};
          filter[1] = (ptype_t){LOWSHELF, bass_fc, bass_gain,       0.707,
                                NULL,     NULL,    {0, 0, 0, 0, 0}, {0, 0}};

          ESP_LOGI(TAG, "got new setting for dspfBassBoost");
        } else {
          ESP_LOGE(TAG, "failed to get memory for filter");
        }

        break;
      }

      case dspfBiamp: {
        cnt = 4;

        filter =
            (ptype_t *)heap_caps_malloc(sizeof(ptype_t) * cnt, MALLOC_CAP_8BIT);
        if (filter) {
          float lp_fc = filterParams.fc_1 / samplerate;
          float lp_gain = filterParams.gain_1;
          float hp_fc = filterParams.fc_3 / samplerate;
          float hp_gain = filterParams.gain_3;

          filter[0] = (ptype_t){LPF,  lp_fc, lp_gain,         0.707,
                                NULL, NULL,  {0, 0, 0, 0, 0}, {0, 0}};
          filter[1] = (ptype_t){LPF,  lp_fc, lp_gain,         0.707,
                                NULL, NULL,  {0, 0, 0, 0, 0}, {0, 0}};
          filter[2] = (ptype_t){HPF,  hp_fc, hp_gain,         0.707,
                                NULL, NULL,  {0, 0, 0, 0, 0}, {0, 0}};
          filter[3] = (ptype_t){HPF,  hp_fc, hp_gain,         0.707,
                                NULL, NULL,  {0, 0, 0, 0, 0}, {0, 0}};

          ESP_LOGI(TAG, "got new setting for dspfBiamp");
        } else {
          ESP_LOGE(TAG, "failed to get memory for filter");
        }

        break;
      }

      case dspf2DOT1: {  // Process audio L + R LOW PASS FILTER
        cnt = 0;
        dspFlow = dspfStereo;

        ESP_LOGW(TAG, "dspf2DOT1, not implemented yet, using stereo instead");
      } break;

      case dspfFunkyHonda: {  // Process audio L + R LOW PASS FILTER
        cnt = 0;
        dspFlow = dspfStereo;

        ESP_LOGW(TAG,
                 "dspfFunkyHonda, not implemented yet, using stereo instead");
        break;
      }

      default: {
        break;
      }
    }

    dsp_processor_gen_filter(filter, cnt);

    init = true;
  }

  // only process data if it is valid
  if (audio_tmp) {
    sbuffer0 = (float *)heap_caps_malloc(sizeof(float) * DSP_PROCESSOR_LEN,
                                         MALLOC_CAP_8BIT);
    if (sbuffer0 == NULL) {
      ESP_LOGE(TAG, "No Memory allocated for dsp_processor sbuffer0");

      return -1;
    }

    sbufout0 = (float *)heap_caps_malloc(sizeof(float) * DSP_PROCESSOR_LEN,
                                         MALLOC_CAP_8BIT);
    if (sbufout0 == NULL) {
      ESP_LOGE(TAG, "No Memory allocated for dsp_processor sbufout0");

      free(sbuffer0);

      return -1;
    }

    switch (dspFlow) {
      case dspfEQBassTreble: {
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);
          uint32_t max = DSP_PROCESSOR_LEN;
          uint32_t test = len - k;

          if (test < DSP_PROCESSOR_LEN) {
            max = test;
          }

          // channel 0
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * /*0.5 **/
                          ((float)((int16_t)(tmp[i] & 0xFFFF))) / INT16_MAX;
          }

          // BASS
          BIQUAD(sbuffer0, sbufout0, max, filter[0].coeffs, filter[0].w);
          // TREBLE
          BIQUAD(sbufout0, sbuffer0, max, filter[1].coeffs, filter[1].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbuffer0[i] * INT16_MAX);
            tmp[i] =
                (volatile uint32_t)((tmp[i] & 0xFFFF0000) + (uint32_t)valint);
          }

          // channel 1
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * /*0.5 **/
                          ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))) /
                          INT16_MAX;
          }

          // BASS
          BIQUAD(sbuffer0, sbufout0, max, filter[2].coeffs, filter[2].w);
          // TREBLE
          BIQUAD(sbufout0, sbuffer0, max, filter[3].coeffs, filter[3].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbuffer0[i] * INT16_MAX);
            tmp[i] = (volatile uint32_t)((tmp[i] & 0xFFFF) +
                                         ((uint32_t)valint << 16));
          }
        }

        break;
      }

      case dspfStereo: {
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);
          uint32_t max = DSP_PROCESSOR_LEN;
          uint32_t test = len - k;

          if (test < DSP_PROCESSOR_LEN) {
            max = test;
          }

          // set volume
          if (dynamic_vol != 1.0) {
            for (i = 0; i < max; i++) {
              tmp[i] =
                  ((uint32_t)(dynamic_vol *
                              ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))))
                   << 16) +
                  (uint32_t)(dynamic_vol *
                             ((float)((int16_t)(tmp[i] & 0xFFFF))));
            }
          }
        }

        break;
      }

      case dspfBassBoost: {  // CH0 low shelf 6dB @ 400Hz
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);
          uint32_t max = DSP_PROCESSOR_LEN;
          uint32_t test = len - k;

          if (test < DSP_PROCESSOR_LEN) {
            max = test;
          }

          // channel 0
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)(tmp[i] & 0xFFFF))) / INT16_MAX;
          }
          BIQUAD(sbuffer0, sbufout0, max, filter[0].coeffs, filter[0].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbufout0[i] * INT16_MAX);
            tmp[i] = (tmp[i] & 0xFFFF0000) + (uint32_t)valint;
          }

          // channel 1
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))) /
                          INT16_MAX;
          }
          BIQUAD(sbuffer0, sbufout0, max, filter[1].coeffs, filter[1].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbufout0[i] * INT16_MAX);
            tmp[i] = (tmp[i] & 0xFFFF) + ((uint32_t)valint << 16);
          }
        }

        break;
      }

      case dspfBiamp: {
        for (int k = 0; k < len; k += DSP_PROCESSOR_LEN) {
          volatile uint32_t *tmp = (uint32_t *)(&audio_tmp[k]);
          uint32_t max = DSP_PROCESSOR_LEN;
          uint32_t test = len - k;

          if (test < DSP_PROCESSOR_LEN) {
            max = test;
          }

          // Process audio ch0 LOW PASS FILTER
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)(tmp[i] & 0xFFFF))) / INT16_MAX;
          }
          BIQUAD(sbuffer0, sbufout0, max, filter[0].coeffs, filter[0].w);
          BIQUAD(sbufout0, sbuffer0, max, filter[1].coeffs, filter[1].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbuffer0[i] * INT16_MAX);
            tmp[i] = (tmp[i] & 0xFFFF0000) + (uint32_t)valint;
          }

          // Process audio ch1 HIGH PASS FILTER
          for (i = 0; i < max; i++) {
            sbuffer0[i] = dynamic_vol * 0.5 *
                          ((float)((int16_t)((tmp[i] & 0xFFFF0000) >> 16))) /
                          INT16_MAX;
          }
          BIQUAD(sbuffer0, sbufout0, max, filter[2].coeffs, filter[2].w);
          BIQUAD(sbufout0, sbuffer0, max, filter[3].coeffs, filter[3].w);

          for (i = 0; i < max; i++) {
            valint = (int16_t)(sbuffer0[i] * INT16_MAX);
            tmp[i] = (tmp[i] & 0xFFFF) + ((uint32_t)valint << 16);
          }
        }

        break;
      }

      case dspf2DOT1: {  // Process audio L + R LOW PASS FILTER
        /*
           BIQUAD(sbuffer2, sbuftmp0, len, bq[0].coeffs, bq[0].w);
           BIQUAD(sbuftmp0, sbufout2, len, bq[1].coeffs, bq[1].w);

           // Process audio L HIGH PASS FILTER
           BIQUAD(sbuffer0, sbuftmp0, len, bq[2].coeffs, bq[2].w);
           BIQUAD(sbuftmp0, sbufout0, len, bq[3].coeffs, bq[3].w);

           // Process audio R HIGH PASS FILTER
           BIQUAD(sbuffer1, sbuftmp0, len, bq[4].coeffs, bq[4].w);
           BIQUAD(sbuftmp0, sbufout1, len, bq[5].coeffs, bq[5].w);

           int16_t valint[5];
           for (uint16_t i = 0; i < len; i++) {
             valint[0] =
                 (muteCH[0] == 1) ? (int16_t)0 : (int16_t)(sbufout0[i] *
           INT16_MAX); valint[1] = (muteCH[1] == 1) ? (int16_t)0 :
           (int16_t)(sbufout1[i] * INT16_MAX); valint[2] = (muteCH[2] == 1) ?
           (int16_t)0 : (int16_t)(sbufout2[i] * INT16_MAX); dsp_audio[i * 4 + 0]
           = (valint[2] & 0xff); dsp_audio[i * 4 + 1] = ((valint[2] & 0xff00) >>
           8); dsp_audio[i * 4 + 2] = 0; dsp_audio[i * 4 + 3] = 0;

             dsp_audio1[i * 4 + 0] = (valint[0] & 0xff);
             dsp_audio1[i * 4 + 1] = ((valint[0] & 0xff00) >> 8);
             dsp_audio1[i * 4 + 2] = (valint[1] & 0xff);
             dsp_audio1[i * 4 + 3] = ((valint[1] & 0xff00) >> 8);
           }

           // TODO: this copy could be avoided if dsp_audio buffers are
           // allocated dynamically and pointers are exchanged after
           // audio was freed
           memcpy(audio, dsp_audio, chunk_size);

           ESP_LOGW(TAG, "Don't know what to do with dsp_audio1");
     */
        ESP_LOGW(TAG, "dspf2DOT1, not implemented yet, using stereo instead");
      } break;

      case dspfFunkyHonda: {  // Process audio L + R LOW PASS FILTER
        /*
          BIQUAD(sbuffer2, sbuftmp0, len, bq[0].coeffs, bq[0].w);
          BIQUAD(sbuftmp0, sbufout2, len, bq[1].coeffs, bq[1].w);

          // Process audio L HIGH PASS FILTER
          BIQUAD(sbuffer0, sbuftmp0, len, bq[2].coeffs, bq[2].w);
          BIQUAD(sbuftmp0, sbufout0, len, bq[3].coeffs, bq[3].w);

          // Process audio R HIGH PASS FILTER
          BIQUAD(sbuffer1, sbuftmp0, len, bq[4].coeffs, bq[4].w);
          BIQUAD(sbuftmp0, sbufout1, len, bq[5].coeffs, bq[5].w);

          uint16_t scale = 16384;  // INT16_MAX
          int16_t valint[5];
          for (uint16_t i = 0; i < len; i++) {
            valint[0] =
                (muteCH[0] == 1) ? (int16_t)0 : (int16_t)(sbufout0[i] * scale);
            valint[1] =
                (muteCH[1] == 1) ? (int16_t)0 : (int16_t)(sbufout1[i] * scale);
            valint[2] =
                (muteCH[2] == 1) ? (int16_t)0 : (int16_t)(sbufout2[i] * scale);
            valint[3] = valint[0] + valint[2];
            valint[4] = -valint[2];
            valint[5] = -valint[1] - valint[2];
            dsp_audio[i * 4 + 0] = (valint[3] & 0xff);
            dsp_audio[i * 4 + 1] = ((valint[3] & 0xff00) >> 8);
            dsp_audio[i * 4 + 2] = (valint[2] & 0xff);
            dsp_audio[i * 4 + 3] = ((valint[2] & 0xff00) >> 8);

            dsp_audio1[i * 4 + 0] = (valint[4] & 0xff);
            dsp_audio1[i * 4 + 1] = ((valint[4] & 0xff00) >> 8);
            dsp_audio1[i * 4 + 2] = (valint[5] & 0xff);
            dsp_audio1[i * 4 + 3] = ((valint[5] & 0xff00) >> 8);
          }

          // TODO: this copy could be avoided if dsp_audio buffers are
          // allocated dynamically and pointers are exchanged after
          // audio was freed
          memcpy(audio, dsp_audio, chunk_size);

          ESP_LOGW(TAG, "Don't know what to do with dsp_audio1");
          */
        ESP_LOGW(TAG,
                 "dspfFunkyHonda, not implemented yet, using stereo instead");
      } break;

      default: {
      } break;
    }

    free(sbuffer0);
    sbuffer0 = NULL;

    free(sbufout0);
    sbufout0 = NULL;
  }

  return 0;
}

// void dsp_set_xoverfreq(uint8_t freqh, uint8_t freql, uint32_t samplerate) {
//  float freq = freqh * 256 + freql;
//  //  printf("%f\n", freq);
//  float f = freq / samplerate / 2.;
//  for (int8_t n = 0; n <= 5; n++) {
//    bq[n].freq = f;
//    switch (bq[n].filtertype) {
//      case LPF:
//        //        for (uint8_t i = 0; i <= 4; i++) {
//        //          printf("%.6f ", bq[n].coeffs[i]);
//        //        }
//        //        printf("\n");
//        dsps_biquad_gen_lpf_f32(bq[n].coeffs, bq[n].freq, bq[n].q);
//        //        for (uint8_t i = 0; i <= 4; i++) {
//        //          printf("%.6f ", bq[n].coeffs[i]);
//        //        }
//        //        printf("%f \n", bq[n].freq);
//        break;
//      case HPF:
//        dsps_biquad_gen_hpf_f32(bq[n].coeffs, bq[n].freq, bq[n].q);
//        break;
//      default:
//        break;
//    }
//  }
//}

/*
 * Format and log filter params as info.
 */
void dsp_processor_log_filter(filterParams_t filterParams) {
  ESP_LOGI(TAG, "Band 1: %fdB @ %fHz", filterParams.gain_1, filterParams.fc_1);
  ESP_LOGI(TAG, "Band 2: %fdB @ %fHz", filterParams.gain_2, filterParams.fc_2);
  ESP_LOGI(TAG, "Band 3: %fdB @ %fHz", filterParams.gain_3, filterParams.fc_3);
  return;
}

/**
 *
 */
void dsp_processor_set_volome(double volume) {
  if (volume >= 0 && volume <= 1.0) {
    ESP_LOGI(TAG, "Set volume to %f", volume);
    dynamic_vol = volume;
  }
}
#endif
