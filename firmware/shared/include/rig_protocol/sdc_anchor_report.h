#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct node_sdc_anchor_report {
  uint16_t connection_handle;
  uint16_t event_counter;
  uint64_t anchor_point_us;
} node_sdc_anchor_report_t;

int node_sdc_enable_anchor_reports(void);

int node_sdc_is_anchor_report(uint8_t subevent);

int node_sdc_decode_anchor_report(const uint8_t *data, size_t length,
                                   node_sdc_anchor_report_t *output);

#ifdef __cplusplus
}
#endif