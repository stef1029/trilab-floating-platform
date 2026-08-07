#include "fairy_shared/sdc_anchor_report.h"

#include <stdbool.h>
#include <string.h>

#include <bluetooth/hci_vs_sdc.h>

int fairy_sdc_enable_anchor_reports(void) {
  sdc_hci_cmd_vs_conn_anchor_point_update_event_report_enable_t parameters = {
      .enable = true,
  };

  return hci_vs_sdc_conn_anchor_point_update_event_report_enable(&parameters);
}

int fairy_sdc_is_anchor_report(uint8_t subevent) {
  return subevent == SDC_HCI_SUBEVENT_VS_CONN_ANCHOR_POINT_UPDATE_REPORT;
}

int fairy_sdc_decode_anchor_report(const uint8_t *data, size_t length,
                                   fairy_sdc_anchor_report_t *output) {
  if (data == NULL || output == NULL ||
      length < sizeof(sdc_hci_subevent_vs_conn_anchor_point_update_report_t)) {
    return 0;
  }

  sdc_hci_subevent_vs_conn_anchor_point_update_report_t event;
  memcpy(&event, data, sizeof(event));

  output->connection_handle = event.conn_handle;
  output->event_counter = event.event_counter;
  output->anchor_point_us = event.anchor_point_us;

  return 1;
}