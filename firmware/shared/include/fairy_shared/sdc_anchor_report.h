#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fairy_sdc_anchor_report {
  uint16_t connection_handle;
  uint16_t event_counter;
  uint64_t anchor_point_us;
} fairy_sdc_anchor_report_t;

int fairy_sdc_enable_anchor_reports(void);

int fairy_sdc_is_anchor_report(uint8_t subevent);

int fairy_sdc_decode_anchor_report(const uint8_t *data, size_t length,
                                   fairy_sdc_anchor_report_t *output);

#ifdef __cplusplus
}
#endif