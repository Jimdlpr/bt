/******************************************************************************
 *
 *  Copyright (C) 2014 Google, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/
#include <string.h>

#include <aos/kernel.h>
#include <aos/debug.h>
#include "common/bt_defs.h"
#include "common/bt_trace.h"
#include "stack/bt_types.h"
#include "hci/buffer_allocator.h"
#include "osi/fixed_queue.h"
#include "hci/hci_hal.h"
#include "hci/hci_internals.h"
#include "hci/hci_layer.h"
#include "osi/thread.h"
#include "stack/hcimsgs.h"

#include <devices/uart.h>
//#include "drv_usart.h"

#if (C2H_FLOW_CONTROL_INCLUDED == TRUE)
#include "l2c_int.h"
#endif ///C2H_FLOW_CONTROL_INCLUDED == TRUE

#ifndef CONFIG_HCI_UART_ID
#define CONFIG_HCI_UART_ID 0
#endif

#ifndef CONFIG_HCI_UART_BAUDRATE
#define CONFIG_HCI_UART_BAUDRATE 115200
#endif

#define HCI_HAL_SERIAL_BUFFER_SIZE 1026
#define HCI_BLE_EVENT 0x3e
#define PACKET_TYPE_TO_INBOUND_INDEX(type) ((type) - 2)
#define PACKET_TYPE_TO_INDEX(type) ((type) - 1)
extern bool BTU_check_queue_is_congest(void);


static const uint8_t preamble_sizes[] = {
    HCI_COMMAND_PREAMBLE_SIZE,
    HCI_ACL_PREAMBLE_SIZE,
    HCI_SCO_PREAMBLE_SIZE,
    HCI_EVENT_PREAMBLE_SIZE
};

static const uint16_t outbound_event_types[] = {
    MSG_HC_TO_STACK_HCI_ERR,
    MSG_HC_TO_STACK_HCI_ACL,
    MSG_HC_TO_STACK_HCI_SCO,
    MSG_HC_TO_STACK_HCI_EVT
};

typedef struct {
    const allocator_t *allocator;
    size_t buffer_size;
    //fixed_queue_t *rx_q;
} hci_hal_env_t;


struct bt_hci_evt_hdr {
	uint8_t  evt;
	uint8_t  len;
};

struct bt_hci_acl_hdr {
	uint16_t handle;
	uint16_t len;
};

typedef struct rx_t {
    uint8_t *buf;
    uint16_t cur_len;

    uint16_t remaining;
    uint16_t discard;

    uint8_t     have_hdr;
    uint8_t     discardable;

    uint8_t  hdr_len;

    uint8_t  type;
    union {
        struct bt_hci_evt_hdr evt;
        struct bt_hci_acl_hdr acl;
        uint8_t hdr[4];
    };

    uint8_t ongoing;
} rx_t;

typedef struct h4_dev_t {
    dev_t *dev;

    rx_t rx;
} h4_dev_t;


static h4_dev_t h4_dev;


static hci_hal_env_t hci_hal_env;
static const hci_hal_t interface;
static const hci_hal_callbacks_t *callbacks;
//static const yoc_vhci_host_callback_t vhci_host_cb;

static aos_task_t hcih4_task_handler;
static int task_run;
static aos_queue_t hcih4_queue;
static char queue_buf[HCI_H4_QUEUE_LEN * sizeof(BtTaskEvt_t)];

//static void host_send_pkt_available_cb(void);
static int host_recv_pkt_cb(uint8_t *data, uint16_t len);

static void hci_hal_h4_rx_handler(void *arg);

#define CONFIG_WIRESHARK_DUMP 1
#ifndef CONFIG_WIRESHARK_DUMP
#define CONFIG_WIRESHARK_DUMP 0
#endif

#if CONFIG_WIRESHARK_DUMP == TRUE
void tcpdump_buffer_print(unsigned int timestamp, unsigned char *buf, int len)
{
	int j = 0;

    /** print %H:%M:%S.*/
    {
        int seconds = timestamp / 1000;
        timestamp = timestamp % 1000;

        int hours = seconds / 3600;
        seconds = seconds % 3600;

        int minutes = seconds / 60;
        seconds = seconds % 60;

        printf("\n%02d:%02d:%02d.%04d",
            hours, minutes, seconds, timestamp);
    }


	for(j=0; j<len; j++)
	{
		if(0 == (j & (16 - 1))) //print 16 hex every line
			printf("\n%06x ", j);
		printf(" %02x",buf[j]&0xff);
	}
	printf("\n");
}
#endif


static void hci_hal_env_init(
    size_t buffer_size,
    size_t max_buffer_count)
{
    assert(buffer_size > 0);
    assert(max_buffer_count > 0);

    hci_hal_env.allocator = buffer_allocator_get_interface();
    hci_hal_env.buffer_size = buffer_size;

#if 0
    hci_hal_env.rx_q = fixed_queue_new(max_buffer_count);
    if (hci_hal_env.rx_q) {
        fixed_queue_register_dequeue(hci_hal_env.rx_q, event_uart_has_bytes);
    } else {
        HCI_TRACE_ERROR("%s unable to create rx queue.\n", __func__);
    }
#endif

    return;
}

static uint32_t read_byte(uint8_t *data, uint32_t len)
{
    int32_t read_len;

    read_len = uart_recv(h4_dev.dev, data, len, 0);

    BTTRC_DUMP_BUFFER("recv data", data, read_len);

    if (read_len == 0) {
        h4_dev.rx.ongoing = 0;
    }

    return read_len;
}

static uint32_t write_byte(uint8_t *data, uint32_t len)
{
    int ret;

    HCI_TRACE_DEBUG("Send %p (len %u)", data, len);

    ret = uart_send(h4_dev.dev, data, len);

    if (ret) {
        return ret;
    }

    return 0;
}

static inline void h4_get_type(void)
{
    /* Get packet type */
    if (read_byte(&h4_dev.rx.type, 1) != 1) {
        h4_dev.rx.type = DATA_TYPE_NONE;
        return;
    }

    switch (h4_dev.rx.type) {
        case DATA_TYPE_EVENT:
            h4_dev.rx.remaining = sizeof(h4_dev.rx.evt);
            h4_dev.rx.hdr_len = h4_dev.rx.remaining;
            break;

        case DATA_TYPE_ACL:
            h4_dev.rx.remaining = sizeof(h4_dev.rx.acl);
            h4_dev.rx.hdr_len = h4_dev.rx.remaining;
            break;

        default:
            HCI_TRACE_ERROR("Unknown H:4 type 0x%02x", h4_dev.rx.type);
            h4_dev.rx.type = DATA_TYPE_NONE;
    }
}


static inline void get_acl_hdr(void)
{
    struct bt_hci_acl_hdr *hdr = &h4_dev.rx.acl;
    int to_read = sizeof(*hdr) - h4_dev.rx.remaining;

    h4_dev.rx.remaining -= read_byte((uint8_t *)hdr + to_read,
                                     h4_dev.rx.remaining);

    if (!h4_dev.rx.remaining) {
        h4_dev.rx.remaining = hdr->len;
        HCI_TRACE_DEBUG("Got ACL header. Payload %u bytes", h4_dev.rx.remaining);
        h4_dev.rx.have_hdr = true;
    }
}


static inline void get_evt_hdr(void)
{
    struct bt_hci_evt_hdr *hdr = &h4_dev.rx.evt;
    int to_read = h4_dev.rx.hdr_len - h4_dev.rx.remaining;

    h4_dev.rx.remaining -= read_byte((uint8_t *)hdr + to_read,
                                     h4_dev.rx.remaining);

    if (!h4_dev.rx.remaining) {
        h4_dev.rx.remaining = hdr->len - (h4_dev.rx.hdr_len - sizeof(*hdr));

        HCI_TRACE_DEBUG("Got event header. Payload %u bytes", hdr->len);
        h4_dev.rx.have_hdr = true;
    }
}


static inline void copy_hdr(uint8_t *buf)
{
    memcpy(buf, h4_dev.rx.hdr, h4_dev.rx.hdr_len);
}

static inline void set_hdr_type(uint8_t *buf)
{
    //memcpy(buf, h4_dev.rx.type, 1);
    *buf = h4_dev.rx.type;
}

static void reset_rx(void)
{
    h4_dev.rx.type = DATA_TYPE_NONE;
    h4_dev.rx.remaining = 0;
    h4_dev.rx.have_hdr = false;
    h4_dev.rx.hdr_len = 0;
    h4_dev.rx.cur_len = 0;
    h4_dev.rx.discardable = false;
}

#if 0
static struct net_buf *get_rx(int timeout)
{
    HCI_TRACE_DEBUG("type 0x%02x, evt 0x%02x", h4_dev.rx.type, h4_dev.rx.evt.evt);

    if (h4_dev.rx.type == DATA_TYPE_EVENT && (h4_dev.rx.evt.evt == BT_HCI_EVT_CMD_COMPLETE ||
                                     h4_dev.rx.evt.evt == BT_HCI_EVT_CMD_STATUS)) {
        return bt_buf_get_cmd_complete(timeout);
    }

    if (h4_dev.rx.type == DATA_TYPE_ACL) {
        return bt_buf_get_rx(BT_BUF_ACL_IN, timeout);
    } else {
        return bt_buf_get_rx(BT_BUF_EVT, timeout);
    }
}
#endif

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

static size_t h4_discard(size_t len)
{
    uint8_t buf[33];

    return read_byte(buf, min(len, sizeof(buf)));
}

static inline void read_payload(void)
{
    int read;

    if (!h4_dev.rx.buf) {
        h4_dev.rx.buf = hci_hal_env.allocator->alloc(h4_dev.rx.remaining +
                                                     h4_dev.rx.hdr_len + 1);

        if (!h4_dev.rx.buf) {
            if (h4_dev.rx.discardable) {
                HCI_TRACE_DEBUG("Discarding event 0x%02x", h4_dev.rx.evt.evt);
                h4_dev.rx.discard = h4_dev.rx.remaining;
                reset_rx();
                return;
            }

            HCI_TRACE_DEBUG("Failed to allocate, deferring to rx_thread");
            return;
        }

        HCI_TRACE_DEBUG("Allocated h4_dev.rx.buf %p", h4_dev.rx.buf);

#if 0
        if (h4_dev.rx.remaining > net_buf_tailroom(h4_dev.rx.buf)) {
            HCI_TRACE_ERROR("Not enough space in buffer");
            h4_dev.rx.discard = h4_dev.rx.remaining;
            reset_rx();
            return;
        }
#endif
        set_hdr_type(h4_dev.rx.buf);
        copy_hdr(h4_dev.rx.buf + 1);
        h4_dev.rx.cur_len = h4_dev.rx.hdr_len + 1;
    }

    read = read_byte(h4_dev.rx.buf + h4_dev.rx.cur_len, h4_dev.rx.remaining);
    h4_dev.rx.cur_len += read;
    h4_dev.rx.remaining -= read;

    HCI_TRACE_DEBUG("got %d bytes, remaining %u", read, h4_dev.rx.remaining);

    if (h4_dev.rx.remaining) {
        return;
    }

    host_recv_pkt_cb(h4_dev.rx.buf, h4_dev.rx.cur_len);

    reset_rx();

    h4_dev.rx.buf = NULL;
}

static inline void read_header(void)
{
    switch (h4_dev.rx.type) {
        case DATA_TYPE_NONE:
            h4_get_type();
            return;

        case DATA_TYPE_EVENT:
            get_evt_hdr();
            break;
        case DATA_TYPE_ACL:
            get_acl_hdr();
            break;

        default:
            reset_rx();
            return;
    }
}

static inline void process_rx(void)
{
    HCI_TRACE_DEBUG("remaining %u discard %u have_hdr %u h4_dev.rx.buf %p",
           h4_dev.rx.remaining, h4_dev.rx.discard, h4_dev.rx.have_hdr, h4_dev.rx.buf);

    if (h4_dev.rx.discard) {
        h4_dev.rx.discard -= h4_discard(h4_dev.rx.discard);
        return;
    }

    if (h4_dev.rx.have_hdr) {
        read_payload();
    } else {
        read_header();
    }
}


static void recv_data()
{
    do {
        h4_dev.rx.ongoing = 1;
        process_rx();
    } while (h4_dev.rx.ongoing);
}

static void uart_event(dev_t *dev, int event_id, void *priv)
{
    if (event_id == USART_EVENT_READ) {
        hci_hal_h4_task_post(SIG_HCI_HAL_RECV_AVAILABLE, 100);
    }
}

static dev_t *dev_uart_open()
{
    uart_config_t config;
    dev_t *uart_dev = uart_open_id("uart", CONFIG_HCI_UART_ID);

    if (uart_dev != NULL) {
        uart_config_default(&config);
        config.baud_rate = CONFIG_HCI_UART_BAUDRATE;
        //config.parity = PARITY_EVEN;
        uart_config(uart_dev, &config);

        uart_set_event(uart_dev, uart_event, NULL);
    }

    return uart_dev;
}

static void hci_hal_env_deinit(void)
{
    //fixed_queue_free(hci_hal_env.rx_q, hci_hal_env.allocator->free);
    //hci_hal_env.rx_q = NULL;
}

static bool hal_open(const hci_hal_callbacks_t *upper_callbacks)
{
    int ret;
    assert(upper_callbacks != NULL);
    callbacks = upper_callbacks;

    memset(&h4_dev, 0, sizeof(h4_dev));


    ret = aos_queue_new(&hcih4_queue, queue_buf, HCI_H4_QUEUE_LEN * sizeof(BtTaskEvt_t), sizeof(BtTaskEvt_t));

    if (ret != 0) {
        return false;
    }

    task_run = 1;

    ret = aos_task_new_ext(&hcih4_task_handler, HCI_H4_TASK_NAME, hci_hal_h4_rx_handler,  NULL,    HCI_H4_TASK_STACK_SIZE, HCI_H4_TASK_PRIO);

    if (ret != 0) {
        aos_queue_free(&hcih4_queue);
        return false;
    }

    h4_dev.dev = dev_uart_open();

    aos_check(h4_dev.dev, EINVAL);

    hci_hal_env_init(HCI_HAL_SERIAL_BUFFER_SIZE, QUEUE_SIZE_MAX);

    return true;
}

static void hal_close()
{
    BtTaskEvt_t evt;
    hci_hal_env_deinit();

    /* delete task and queue */
    //vTaskDelete(xHciH4TaskHandle);
    task_run = 0;

    evt.sig = SIG_HCI_HAL_NUM;
    evt.par = 0;

    aos_queue_send(&hcih4_queue, &evt, sizeof(BtTaskEvt_t));

    aos_queue_free(&hcih4_queue);
}

/**
 * Function: transmit_data -TX data to low-layer
 * It is ported from Bluedroid source code, so it is not
 * needed to use write() to send data.
 * TODO: Just use firmware API to send data.
 */
static uint16_t transmit_data(serial_data_type_t type,
                              uint8_t *data, uint16_t length)
{
    uint8_t previous_byte;

    assert(data != NULL);
    assert(length > 0);

    if (type < DATA_TYPE_COMMAND || type > DATA_TYPE_SCO) {
        HCI_TRACE_ERROR("%s invalid data type: %d", __func__, type);
        return 0;
    }

    // Write the signal byte right before the data
    --data;
    previous_byte = *data;
    *(data) = type;
    ++length;

    BTTRC_DUMP_BUFFER("Transmit Pkt", data, length);
#if CONFIG_WIRESHARK_DUMP == TRUE
    tcpdump_buffer_print(aos_now_ms(), data, length);
#endif
    // TX Data to target
    //yoc_vhci_host_send_packet(data, length);

    write_byte(data, length);

    // Be nice and restore the old value of that byte
    *(data) = previous_byte;

    return length - 1;
}

// Internal functions
static void hci_hal_h4_rx_handler(void *arg)
{
    BtTaskEvt_t e;
    unsigned int len;

    while (task_run) {
        if (0 == aos_queue_recv(&hcih4_queue, AOS_WAIT_FOREVER, &e, &len)) {
#if 0
            if (e.sig == SIG_HCI_HAL_RECV_PACKET) {
                fixed_queue_process(hci_hal_env.rx_q);

            } else 
#endif
            if (e.sig == SIG_HCI_HAL_RECV_AVAILABLE) {
                recv_data();
            }
        }
    }
}

task_post_status_t hci_hal_h4_task_post(SIG_HCI_HAL_t type, task_post_t timeout)
{
    BtTaskEvt_t evt;

    evt.sig = type;
    evt.par = 0;

    if (aos_queue_send(&hcih4_queue, &evt, sizeof(BtTaskEvt_t)) == 0) {
        return TASK_POST_SUCCESS;
    }

    return TASK_POST_FAIL;
}

#if (C2H_FLOW_CONTROL_INCLUDED == TRUE)
static void hci_packet_complete(BT_HDR *packet){
    uint8_t type, num_handle;
    uint16_t handle;
    uint16_t handles[MAX_L2CAP_LINKS + 4];
    uint16_t num_packets[MAX_L2CAP_LINKS + 4];
    uint8_t *stream = packet->data + packet->offset;
    tL2C_LCB  *p_lcb = NULL;

    STREAM_TO_UINT8(type, stream);
    if (type == DATA_TYPE_ACL/* || type == DATA_TYPE_SCO*/) {
        STREAM_TO_UINT16(handle, stream);
        handle = handle & HCI_DATA_HANDLE_MASK;
        p_lcb = l2cu_find_lcb_by_handle(handle);
        if (p_lcb) {
            p_lcb->completed_packets++;
        }
        //if (yoc_vhci_host_check_send_available()){
            num_handle = l2cu_find_completed_packets(handles, num_packets);
            if (num_handle > 0){
                btsnd_hcic_host_num_xmitted_pkts (num_handle, handles, num_packets);
            }
        //} else {
            //Send HCI_Host_Number_of_Completed_Packets next time.
       // }

    }
}
#endif ///C2H_FLOW_CONTROL_INCLUDED == TRUE

bool host_recv_adv_packet(BT_HDR *packet)
{
    assert(packet);
        return true;
    if(packet->data[0] == DATA_TYPE_EVENT && packet->data[1] == HCI_BLE_EVENT && packet->data[3] ==  HCI_BLE_ADV_PKT_RPT_EVT) {
    }
    return false;
}

static void hci_hal_h4_hdl_rx_packet(BT_HDR *packet)
{
    uint8_t type, hdr_size;
    uint16_t length;
    uint8_t *stream = packet->data + packet->offset;

    if (!packet) {
        return;
    }

#if (C2H_FLOW_CONTROL_INCLUDED == TRUE)
    hci_packet_complete(packet);
#endif ///C2H_FLOW_CONTROL_INCLUDED == TRUE

    STREAM_TO_UINT8(type, stream);
    packet->offset++;
    packet->len--;
    if (type == HCI_BLE_EVENT) {
        uint8_t len = 0;
        STREAM_TO_UINT8(len, stream);
        HCI_TRACE_ERROR("Workround stream corrupted during LE SCAN: pkt_len=%d ble_event_len=%d\n",
                  packet->len, len);
        hci_hal_env.allocator->free(packet);
        return;
    }
    if (type < DATA_TYPE_ACL || type > DATA_TYPE_EVENT) {
        HCI_TRACE_ERROR("%s Unknown HCI message type. Dropping this byte 0x%x,"
                  " min %x, max %x\n", __func__, type,
                  DATA_TYPE_ACL, DATA_TYPE_EVENT);
        hci_hal_env.allocator->free(packet);
        return;
    }
    hdr_size = preamble_sizes[type - 1];
    if (packet->len < hdr_size) {
        HCI_TRACE_ERROR("Wrong packet length type=%d pkt_len=%d hdr_len=%d",
                  type, packet->len, hdr_size);
        hci_hal_env.allocator->free(packet);
        return;
    }
    if (type == DATA_TYPE_ACL) {
        stream += hdr_size - 2;
        STREAM_TO_UINT16(length, stream);
    } else {
        stream += hdr_size - 1;
        STREAM_TO_UINT8(length, stream);
    }

    if ((length + hdr_size) != packet->len) {
        HCI_TRACE_ERROR("Wrong packet length type=%d hdr_len=%d pd_len=%d "
                  "pkt_len=%d", type, hdr_size, length, packet->len);
        hci_hal_env.allocator->free(packet);
        return;
    }
#if SCAN_QUEUE_CONGEST_CHECK
    if(BTU_check_queue_is_congest() && host_recv_adv_packet(packet)) {
        HCI_TRACE_ERROR("BtuQueue is congested");
        hci_hal_env.allocator->free(packet);
        return;
    }
#endif

    packet->event = outbound_event_types[PACKET_TYPE_TO_INDEX(type)];
    callbacks->packet_ready(packet);
}
#if 0
static void event_uart_has_bytes(fixed_queue_t *queue)
{
    BT_HDR *packet;
    while (!fixed_queue_is_empty(queue)) {
        packet = fixed_queue_dequeue(queue);
        hci_hal_h4_hdl_rx_packet(packet);
    }
}


static void host_send_pkt_available_cb(void)
{
    //Controller rx cache buffer is ready for receiving new host packet
    //Just Call Host main thread task to process pending packets.
    hci_host_task_post(SIG_HCI_HOST_SEND_AVAILABLE, TASK_POST_BLOCKING);
}
#endif

static int host_recv_pkt_cb(uint8_t *data, uint16_t len)
{
    //Target has packet to host, malloc new buffer for packet
    BT_HDR *pkt;
    size_t pkt_size;

    //if (hci_hal_env.rx_q == NULL) {
    //    return 0;
    //}

    pkt_size = BT_HDR_SIZE + len;
    pkt = (BT_HDR *)hci_hal_env.allocator->alloc(pkt_size);
    if (!pkt) {
        HCI_TRACE_ERROR("%s couldn't aquire memory for inbound data buffer.\n", __func__);
        return -1;
    }
    pkt->offset = 0;
    pkt->len = len;
    pkt->layer_specific = 0;
    memcpy(pkt->data, data, len);

    hci_hal_env.allocator->free(data);

    BTTRC_DUMP_BUFFER("Recv Pkt", pkt->data, len);

#if CONFIG_WIRESHARK_DUMP == TRUE
    tcpdump_buffer_print(aos_now_ms(), pkt->data, len);
#endif

    hci_hal_h4_hdl_rx_packet(pkt);
    //fixed_queue_enqueue(hci_hal_env.rx_q, pkt);
    //hci_hal_h4_task_post(SIG_HCI_HAL_RECV_PACKET, 100);

    return 0;
}

#if 0
static const yoc_vhci_host_callback_t vhci_host_cb = {
    .notify_host_send_available = host_send_pkt_available_cb,
    .notify_host_recv = host_recv_pkt_cb,
};
#endif

static const hci_hal_t interface = {
    hal_open,
    hal_close,
    transmit_data,
};

const hci_hal_t *hci_hal_h4_get_interface()
{
    return &interface;
}

