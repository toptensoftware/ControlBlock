// Native USB device: descriptors, the vendor interface and its control
// requests. Spec: docs/protocol.md, USB layer.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "hid_keyboard.h"
#include "protocol.h"
#include "usb.h"

static const char *TAG = "usb";

// ---------------------------------------------------------------------------
// Interfaces and endpoints
//
// A composite device from the start (class EF/02/01), so later modes add
// interfaces without changing what's here:
//   interface 0      vendor: host protocol (mode 1)      EP 0x01 OUT, 0x81 IN
//   interface 1      HID keyboard (standalone)           EP 0x82 IN
//   (mode 2/3)       MIDI streaming (+ audio control)    EP 0x03 OUT, 0x83 IN
// To add one: enable it in sdkconfig (CONFIG_TINYUSB_HID_COUNT /
// CONFIG_TINYUSB_MIDI_COUNT), add its interface number(s) to the enum, its
// descriptor to s_config, its length to CONFIG_TOTAL_LEN, a string, and its
// class callbacks (e.g. tud_hid_descriptor_report_cb). Bump the PID when the
// interface set changes: Windows caches drivers per VID/PID.
//
// With one interface Windows treats the device as single function (no
// composite driver), so the MS OS 2.0 features are at device level. With a
// second interface it becomes composite: the vendor interface's features are
// wrapped in configuration and function subset headers (see TinyUSB's
// webusb_serial example), or Windows ignores them.
//
// The HID keyboard is all under USB_HID_KEYBOARD (CONFIG_TINYUSB_HID_COUNT in
// sdkconfig.defaults). To back it out: set the count to 0, delete
// build/<preset>/sdkconfig and bump USB_BCD_DEVICE; the descriptors then
// come out as the vendor-only device.
// ---------------------------------------------------------------------------

#define USB_HID_KEYBOARD (CONFIG_TINYUSB_HID_COUNT > 0)

enum
{
    ITF_VENDOR = 0,
#if USB_HID_KEYBOARD
    ITF_HID,
#endif
    ITF_COUNT
};

#define EP_VENDOR_OUT    0x01
#define EP_VENDOR_IN     0x81
#define EP_SIZE_FS       64

// Espressif's VID with a development PID (vendor interface only). Apply to
// Espressif for a PID before release (docs/protocol.md).
#define USB_VID          0x303A
#define USB_PID          0x4020
// Bump when the MS OS 2.0 descriptor changes: Windows caches it per
// VID/PID/bcdDevice (0x0100 had it in a function subset, which a single
// function device ignores; 0x0102 added the HID keyboard, so composite)
#define USB_BCD_DEVICE   0x0102

// Vendor request codes: the BOS descriptor tells the host which to use
#define VENDOR_REQ_WEBUSB    0x01
#define VENDOR_REQ_MS_OS_20  0x02
static_assert(PROTO_REQ_RESET_STREAM != VENDOR_REQ_WEBUSB && PROTO_REQ_RESET_STREAM != VENDOR_REQ_MS_OS_20,
              "vendor request codes clash");

enum
{
    STR_LANGID = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_VENDOR_ITF,
    STR_HID_ITF,
    STR_COUNT
};

static const tusb_desc_device_t s_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,           // 2.1: BOS descriptor (WebUSB, MS OS 2.0)
    .bDeviceClass       = TUSB_CLASS_MISC,  // composite (IAD)
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = USB_BCD_DEVICE,
    .iManufacturer      = STR_MANUFACTURER,
    .iProduct           = STR_PRODUCT,
    .iSerialNumber      = STR_SERIAL,
    .bNumConfigurations = 1,
};

#if USB_HID_KEYBOARD
#define EP_HID_IN        0x82
#define HID_POLL_MS      2
#define HID_HW_LEN       TUD_HID_DESC_LEN
// Boot-protocol keyboard, no report ID: the 8-byte reports hid_keyboard.h builds
static const uint8_t s_hid_report[] = { TUD_HID_REPORT_DESC_KEYBOARD() };
#else
#define HID_HW_LEN       0
#endif

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + HID_HW_LEN)

static const uint8_t s_config[] = {
    // Config number, interface count, string index, total length, attributes, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0x00, 500),
    // Interface number, string index, EP out, EP in, EP size
    TUD_VENDOR_DESCRIPTOR(ITF_VENDOR, STR_VENDOR_ITF, EP_VENDOR_OUT, EP_VENDOR_IN, EP_SIZE_FS),
#if USB_HID_KEYBOARD
    // Interface number, string index, boot protocol, report descriptor length, EP in, EP size, interval
    TUD_HID_DESCRIPTOR(ITF_HID, STR_HID_ITF, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_hid_report), EP_HID_IN, HID_KB_REPORT_BYTES, HID_POLL_MS),
#endif
};

// ---------------------------------------------------------------------------
// BOS: WebUSB (no landing page) and Microsoft OS 2.0, so Windows binds
// WinUSB to the vendor interface with no driver install
// ---------------------------------------------------------------------------

#define MS_OS_20_FEATURES_LEN   0x98        // the compatible ID and registry property below
#if USB_HID_KEYBOARD
// Composite: the features sit in a function subset for the vendor interface
// (configuration subset header, then function subset header)
#define MS_OS_20_FUNCTION_LEN   (0x08 + MS_OS_20_FEATURES_LEN)
#define MS_OS_20_CONFIG_LEN     (0x08 + MS_OS_20_FUNCTION_LEN)
#define MS_OS_20_DESC_LEN       (0x0A + MS_OS_20_CONFIG_LEN)
#else
#define MS_OS_20_DESC_LEN       (0x0A + MS_OS_20_FEATURES_LEN)
#endif
#define BOS_TOTAL_LEN     (TUD_BOS_DESC_LEN + TUD_BOS_WEBUSB_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t s_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 2),
    TUD_BOS_WEBUSB_DESCRIPTOR(VENDOR_REQ_WEBUSB, 0),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQ_MS_OS_20),
};

// UTF-16LE, one char at a time
#define W(c) (c), 0x00

static const uint8_t s_ms_os_20[] = {
    // Set header: length, type, Windows version (8.1+), total length
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000),
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

#if USB_HID_KEYBOARD
    // Configuration subset (configuration 0), then function subset (the vendor interface)
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0x00, 0x00,
    U16_TO_U8S_LE(MS_OS_20_CONFIG_LEN),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF_VENDOR, 0x00,
    U16_TO_U8S_LE(MS_OS_20_FUNCTION_LEN),
#endif

    // Compatible ID: WINUSB (at device level when single function, else in the function subset)
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // Registry property: DeviceInterfaceGUIDs (REG_MULTI_SZ), so libusb and
    // WinUSB clients can find the interface
    U16_TO_U8S_LE(MS_OS_20_FEATURES_LEN - 0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    W('D'), W('e'), W('v'), W('i'), W('c'), W('e'), W('I'), W('n'), W('t'), W('e'), W('r'), W('f'),
    W('a'), W('c'), W('e'), W('G'), W('U'), W('I'), W('D'), W('s'), W(0),
    U16_TO_U8S_LE(0x0050),
    // {8A1F3C52-6D4E-4B7A-9C21-5E0F7D3B6A19}, ControlBlock's own
    W('{'), W('8'), W('A'), W('1'), W('F'), W('3'), W('C'), W('5'), W('2'), W('-'),
    W('6'), W('D'), W('4'), W('E'), W('-'), W('4'), W('B'), W('7'), W('A'), W('-'),
    W('9'), W('C'), W('2'), W('1'), W('-'), W('5'), W('E'), W('0'), W('F'), W('7'),
    W('D'), W('3'), W('B'), W('6'), W('A'), W('1'), W('9'), W('}'), W(0), W(0),
};
static_assert(sizeof(s_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor length");

static const char s_langid[] = { 0x09, 0x04 };     // English (US)

static const char *s_strings[STR_COUNT] = {
    s_langid,
    "Topten Software",
    "ControlBlock",
    nullptr,                            // serial: set in usb_init
    "ControlBlock Host",
    "ControlBlock Keyboard",
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static usb_callbacks_t   s_cb;
static SemaphoreHandle_t s_rx_lock;     // a read vs a RESET_STREAM flush
static SemaphoreHandle_t s_tx_room;     // given when an IN transfer completes
static volatile uint32_t s_epoch;

#define WRITE_TIMEOUT_US 1000000        // the host isn't reading: give up

// ---------------------------------------------------------------------------
// TinyUSB callbacks (TinyUSB task)
// ---------------------------------------------------------------------------

extern "C" uint8_t const *tud_descriptor_bos_cb(void)
{
    return s_bos;
}

extern "C" bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    if (stage != CONTROL_STAGE_SETUP)
        return true;
    if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR)
        return false;

    switch (req->bRequest) {
    case VENDOR_REQ_MS_OS_20:
        if (req->wIndex != 7)           // MS_OS_20_DESCRIPTOR_INDEX
            return false;
        return tud_control_xfer(rhport, req, (void *)(uintptr_t)s_ms_os_20, sizeof(s_ms_os_20));

    case PROTO_REQ_RESET_STREAM:
        if (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE || req->wIndex != ITF_VENDOR)
            return false;
        // Under the read lock, so a reader either got its bytes before this
        // (old epoch) or reads only what comes after (new epoch)
        xSemaphoreTake(s_rx_lock, portMAX_DELAY);
        tud_vendor_read_flush();
        s_epoch = s_epoch + 1;
        xSemaphoreGive(s_rx_lock);
        if (s_cb.reset_stream)
            s_cb.reset_stream();
        return tud_control_status(rhport, req);

    default:
        return false;                   // includes WebUSB GET_URL: there's no landing page
    }
}

extern "C" void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    // The data is in the RX FIFO; the protocol task reads it
    if (s_cb.rx)
        s_cb.rx(bufsize);
}

extern "C" void tud_vendor_tx_cb(uint8_t itf, uint32_t sent_bytes)
{
    xSemaphoreGive(s_tx_room);
}

#if USB_HID_KEYBOARD
extern "C" uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return s_hid_report;
}

// Nothing to report on request, and the host's LED output reports are ignored
extern "C" uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t *, uint16_t)
{
    return 0;
}

extern "C" void tud_hid_set_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t const *, uint16_t)
{
}
#endif

// Unplugged / unmounted, or suspended (CONFIG_TINYUSB_SUSPEND_CALLBACK)
static void usb_event(tinyusb_event_t *event, void *)
{
    switch (event->id) {
    case TINYUSB_EVENT_DETACHED:
#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
    case TINYUSB_EVENT_SUSPENDED:
#endif
        if (s_cb.disconnected)
            s_cb.disconnected();
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

esp_err_t usb_init(const char *serial, const usb_callbacks_t *cb)
{
    s_cb = *cb;
    s_rx_lock = xSemaphoreCreateMutex();
    s_tx_room = xSemaphoreCreateBinary();
    if (!s_rx_lock || !s_tx_room)
        return ESP_ERR_NO_MEM;
    s_strings[STR_SERIAL] = serial;

    tinyusb_config_t cfg = {};
    cfg.port                         = TINYUSB_PORT_FULL_SPEED_0;
    cfg.phy.skip_setup               = false;
    cfg.phy.self_powered             = false;
    cfg.phy.vbus_monitor_io          = -1;
    cfg.task.size                    = 4096;
    cfg.task.priority                = 5;
    cfg.task.xCoreID                 = 0;   // off the core that draws
    cfg.descriptor.device            = &s_device;
    cfg.descriptor.string            = s_strings;
    cfg.descriptor.string_count      = STR_COUNT;
    cfg.descriptor.full_speed_config = s_config;
    cfg.event_cb                     = usb_event;

    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "USB %04x:%04x, serial %s", USB_VID, USB_PID, serial);
    return ESP_OK;
}

size_t usb_vendor_read(void *buf, size_t len, uint32_t *epoch, uint32_t *waiting)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    *epoch = s_epoch;
    *waiting = tud_vendor_available();
    size_t n = *waiting ? tud_vendor_read(buf, (uint32_t)len) : 0;
    xSemaphoreGive(s_rx_lock);
    return n;
}

bool usb_vendor_write(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    int64_t deadline = esp_timer_get_time() + WRITE_TIMEOUT_US;
    while (len) {
        if (!tud_mounted())
            return false;
        uint32_t n = tud_vendor_write(p, (uint32_t)len);
        p   += n;
        len -= n;
        if (!len)
            break;
        // FIFO full: send it and wait for room
        tud_vendor_write_flush();
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "host isn't reading: dropped %u bytes", (unsigned)len);
            return false;
        }
        xSemaphoreTake(s_tx_room, pdMS_TO_TICKS(10));
    }
    return true;
}

void usb_vendor_flush(void)
{
    if (tud_mounted())
        tud_vendor_write_flush();
}

size_t usb_vendor_write_space(void)
{
    return tud_mounted() ? tud_vendor_write_available() : 0;
}

#if USB_HID_KEYBOARD
// The previous report may still be on its way (one IN transfer at a time, a
// poll interval apart): wait a little rather than drop a key up
#define HID_SEND_WAIT_MS 20

bool usb_hid_send(const uint8_t *report)
{
    for (int waited = 0; tud_mounted(); waited++) {
        if (tud_hid_ready())
            return tud_hid_report(0, report, HID_KB_REPORT_BYTES);
        if (waited >= HID_SEND_WAIT_MS)
            break;
        vTaskDelay(1);
    }
    return false;
}
#else
bool usb_hid_send(const uint8_t *) { return false; }
#endif
