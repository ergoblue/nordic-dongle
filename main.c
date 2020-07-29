#include "app_timer.h"
#include "app_usbd.h"
#include "app_usbd_hid_generic.h"
#include "ble_db_discovery.h"
#include "ble_hids.h"
#include "bsp.h"
#include "nrf_ble_gatt.h"
#include "nrf_ble_lesc.h"
#include "nrf_ble_scan.h"
#include "nrf_cli.h"
#include "nrf_drv_clock.h"
#include "nrf_drv_gpiote.h"
#include "nrf_drv_usbd.h"
#include "nrf_log.h"
#include "nrf_log_default_backends.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "peer_manager.h"
#include "peer_manager_handler.h"

// Include file containing HID descriptor under the report_map variable.
#include "report_map.h"

// Define tag for the SoftDevice BLE configuration.
#define APP_BLE_CONN_CFG_TAG 1

// Define service to search for on the connected peripheral.
ble_uuid_t hids_uuid = {
	.uuid = BLE_UUID_HUMAN_INTERFACE_DEVICE_SERVICE,
	.type = BLE_UUID_TYPE_BLE,
};

// Define peripheral MAC address.
static uint8_t peripheral_mac[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Define modules for service discovery and scanning.
NRF_BLE_GQ_DEF(ble_gatt_queue, NRF_SDH_BLE_CENTRAL_LINK_COUNT, NRF_BLE_GQ_QUEUE_SIZE);
BLE_DB_DISCOVERY_DEF(ble_disc);
NRF_BLE_SCAN_DEF(ble_scan);

// Define variables to store passkey state if bonding for the first time. In
// particular, the passkey is "typed" via USB and this keeps track of the number
// of keys typed and whether the key is pressed.
static uint8_t passkey[6];
static uint8_t passkey_i = 0;
static bool passkey_output = false;

// Define variables to store the peripheral's report map value.
static uint8_t report_map_remote[256];
static uint8_t report_map_remote_len = 0;

// Define variables for handles. In particular, the report map handle is used
// for reading the report map characteristic and the report handle is the CCCD
// handle used for notification.
static uint16_t conn_handle, report_map_handle, report_handle;

// Define variables to store queued data.
static uint8_t queue_data[256][8];
static uint8_t queue_lens[256];
static uint8_t queue_count = 0;
static uint8_t queue_i = 0;
static uint8_t queue_pending = false;

static void enable_notify(void) {
	// Enable notification for the report characteristic. This code comes from
	// cccd_configure() in components/ble/ble_services/ble_nus_c/ble_nus_c.c.
	uint8_t data[BLE_CCCD_VALUE_LEN] = {BLE_GATT_HVX_NOTIFICATION, 0};
	ble_gattc_write_params_t const write_params = {
		.write_op = BLE_GATT_OP_WRITE_REQ,
		.flags	= BLE_GATT_EXEC_WRITE_FLAG_PREPARED_WRITE,
		.handle = report_handle,
		.p_value = data,
		.len = sizeof(data),
	};
	APP_ERROR_CHECK(sd_ble_gattc_write(conn_handle, &write_params));
}

static void queue_hid_data(const uint8_t *data, uint8_t len) {
	memcpy(queue_data[queue_i], data, len);
	queue_lens[queue_i] = len;
	queue_count++;
	queue_i++;
}

static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context) {
	switch (p_ble_evt->header.evt_id) {

	// After connecting to peripheral, initialize bonding if peripheral is not
	// bonded. Otherwise complete service discovery.
	case BLE_GAP_EVT_CONNECTED: {
		conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
		pm_peer_id_t peer_id;
		APP_ERROR_CHECK(pm_peer_id_get(conn_handle, &peer_id));
		if (peer_id == PM_PEER_ID_INVALID) {
			APP_ERROR_CHECK(pm_conn_secure(conn_handle, false));
		} else {
			APP_ERROR_CHECK(ble_db_discovery_start(&ble_disc, conn_handle));
		}
		break;
	}

	// When connection disconnects, restart scanning. Reset the remote report
	// map length to 0 so it reads correctly after reconnecting.
	case BLE_GAP_EVT_DISCONNECTED:
		APP_ERROR_CHECK(nrf_ble_scan_start(&ble_scan));
		report_map_remote_len = 0;
		break;

	// Upon receiving a passkey to display, copy passkey and reset passkey
	// output state.
	case BLE_GAP_EVT_PASSKEY_DISPLAY: {
		memcpy(passkey, p_ble_evt->evt.gap_evt.params.passkey_display.passkey, 6);
		passkey_i = 0;
		passkey_output = true;
		break;
	}

	// Accepting connection parameters from controller.
	case BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST: {
		const ble_gap_conn_params_t *params = &p_ble_evt->evt.gap_evt.params.conn_param_update_request.conn_params;
		APP_ERROR_CHECK(sd_ble_gap_conn_param_update(conn_handle, params));
		break;
	}

	case BLE_GATTC_EVT_READ_RSP: {
		// Append data to HID data. This, along with BLE_GATTC_EVT_HVX below,
		// uses a pointer for the event. For some reason, the data address gets
		// changed when copying the actual struct.
		const ble_gattc_evt_read_rsp_t *resp = &p_ble_evt->evt.gattc_evt.params.read_rsp;
		memcpy(&report_map_remote[report_map_remote_len], resp->data, resp->len);
		report_map_remote_len += resp->len;

		// If the last read operation returned data, continue reading with
		// the current length as the offset.
		if (resp->len > 0) {
			APP_ERROR_CHECK(sd_ble_gattc_read(conn_handle, report_map_handle, report_map_remote_len));
		} else {
			// Verify that the report map is identical to the hardcoded one. If
			// so, enable notification for the report descriptor. If not, enable
			// red LED and exit with fatal error. This expects that the project
			// was compiled with DEBUG defined so the dongle stops working
			// instead of resetting and the LED remains on.
			if (report_map_remote_len == sizeof(report_map)) {
				if (memcmp(report_map_remote, report_map, sizeof(report_map)) == 0) {
					enable_notify();
					break;
				}
			}
			bsp_board_led_on(BSP_BOARD_LED_1);
			APP_ERROR_CHECK_BOOL(false);
		}
		break;
	}

	// Upon receiving notification, add data and length to queue.
	case BLE_GATTC_EVT_HVX: {
		const ble_gattc_evt_hvx_t *hvx = &p_ble_evt->evt.gattc_evt.params.hvx;
		queue_hid_data(hvx->data, hvx->len);
		break;
	}

	default:
		break;

	}
}

static void ble_init(void) {
	// Get default BLE configuration and enable BLE stack.
	uint32_t ram_start = 0;
	APP_ERROR_CHECK(nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start));
	APP_ERROR_CHECK(nrf_sdh_ble_enable(&ram_start));

	// Register a handler for BLE events.
	NRF_SDH_BLE_OBSERVER(ble_observer, 3, ble_evt_handler, NULL);
}

static void gatt_init(void) {
	NRF_BLE_GATT_DEF(ble_gatt);
	APP_ERROR_CHECK(nrf_ble_gatt_init(&ble_gatt, NULL));
}

static void pm_evt_handler(pm_evt_t const *p_evt) {
	pm_handler_on_pm_evt(p_evt);
	pm_handler_flash_clean(p_evt);
	pm_handler_disconnect_on_sec_failure(p_evt);

	// If peers were deleted successfully, reboot so we can reestablish bond.
	if (p_evt->evt_id == PM_EVT_PEERS_DELETE_SUCCEEDED) {
		sd_nvic_SystemReset();
	}
}

static void peer_manager_init(void) {
	ble_gap_sec_params_t sec_param = {
		.bond = 1,
		.min_key_size = 16,
		.max_key_size = 16,
		.kdist_own.enc = 1,
		.kdist_own.id = 1,
		.kdist_peer.enc = 1,
		.kdist_peer.id = 1,

		// Enable LE Secure Connections. See https://bit.ly/2Ft9MWT and
		// https://bit.ly/2ZIIL9y for information regarding security of
		// connection established with LESC.
		.lesc = 1,

		// By default, enable MITM protection by requiring a passkey input.
		.mitm = 1,
		.io_caps = BLE_GAP_IO_CAPS_DISPLAY_ONLY,
	};

	APP_ERROR_CHECK(pm_init());
	APP_ERROR_CHECK(pm_sec_params_set(&sec_param));
	APP_ERROR_CHECK(pm_register(pm_evt_handler));
}

static void db_disc_handler(ble_db_discovery_evt_t * p_evt) {
	// Return early if event is not the one we are looking for.
	if (p_evt->evt_type != BLE_DB_DISCOVERY_COMPLETE) {
		return;
	}
	if (p_evt->params.discovered_db.srv_uuid.uuid != hids_uuid.uuid) {
		return;
	}
	if (p_evt->params.discovered_db.srv_uuid.type != hids_uuid.type) {
		return;
	}

	// Loop through all the characteristics and obtain handle for report and
	// report map.
	for (int i = 0; i < p_evt->params.discovered_db.char_count; i++) {
		ble_gatt_db_char_t chr = p_evt->params.discovered_db.charateristics[i];
		uint16_t uuid = chr.characteristic.uuid.uuid;
		switch (uuid) {
		case BLE_UUID_REPORT_MAP_CHAR:
			report_map_handle = chr.characteristic.handle_value;
			break;
		case BLE_UUID_REPORT_CHAR:
			report_handle = chr.cccd_handle;
			break;
		default:
			break;
		}
	}

	// Begin reading report map.
	APP_ERROR_CHECK(sd_ble_gattc_read(conn_handle, report_map_handle, 0));
}

static void scan_init(void) {
	// Initialize scanner.
	nrf_ble_scan_init_t init_scan = {
		.connect_if_match = true,
		.conn_cfg_tag = APP_BLE_CONN_CFG_TAG,
	};
	APP_ERROR_CHECK(nrf_ble_scan_init(&ble_scan, &init_scan, NULL));

	// Configure scanner with peripheral MAC address.
	APP_ERROR_CHECK(nrf_ble_scan_filter_set(&ble_scan, SCAN_ADDR_FILTER, peripheral_mac));
	APP_ERROR_CHECK(nrf_ble_scan_filters_enable(&ble_scan, SCAN_ADDR_FILTER, true));
}

static void scan_timer_handler(void *p_context) {
	APP_ERROR_CHECK(nrf_ble_scan_start(&ble_scan));
}

static void passkey_timer_handler(void *p_context) {
	passkey_output = true;
}

static void passkey_output_char(uint8_t c) {
	uint8_t data[8] = {1};
	data[2] = c;
	queue_hid_data(data, 8);
}

static void hid_evt_handler(app_usbd_class_inst_t const * p_inst, app_usbd_hid_user_event_t event) {
	if (event == APP_USBD_HID_USER_EVT_IN_REPORT_DONE) {
		queue_pending = false;
	}
}

// Initialize USB HID instance. Ideally this would be defined dynamically after
// receiving the report map value from the peripheral. We cannot use the
// APP_USBD_HID_GENERIC_GLOBAL_DEF macro inside a function since internally it
// calls NRF_QUEUE_DEF(), which must be at the top level. Copying the macro's
// code introduces complexity and is not worthwhile as we don't anticipate that
// the report map will change frequently, if at all.
app_usbd_hid_subclass_desc_t report_map_desc = {
	.p_data = report_map,
	.size = sizeof(report_map),
	.type = APP_USBD_DESCRIPTOR_REPORT,
};
static const app_usbd_hid_subclass_desc_t *reps[] = {&report_map_desc};
APP_USBD_HID_GENERIC_GLOBAL_DEF(usb_hid, 0, hid_evt_handler, (NRF_DRV_USBD_EPIN1), reps, 1, 0, 0, APP_USBD_HID_SUBCLASS_BOOT, APP_USBD_HID_PROTO_GENERIC);

static void gpio_evt_handler(nrf_drv_gpiote_pin_t pin, nrf_gpiote_polarity_t action) {
	APP_ERROR_CHECK(pm_peers_delete());
}

static void gpio_init(void) {
	nrf_drv_gpiote_init();
	nrf_drv_gpiote_in_config_t in_config = GPIOTE_CONFIG_IN_SENSE_HITOLO(false);
	in_config.pull = NRF_GPIO_PIN_PULLUP;
	nrf_drv_gpiote_in_init(38, &in_config, gpio_evt_handler);
	nrf_drv_gpiote_in_event_enable(38, true);
}

int main(void) {
	// Initialize logging module. It may not be practical to receive the logs if
	// nothing is connected via GPIO pin 29.
	APP_ERROR_CHECK(NRF_LOG_INIT(NULL));
	NRF_LOG_DEFAULT_BACKENDS_INIT();

	// This appears necessary before initializing USB.
	APP_ERROR_CHECK(nrf_drv_clock_init());
	nrf_drv_clock_lfclk_request(NULL);
	while (!nrf_drv_clock_lfclk_is_running());

	// Initialize USB. This must be done before initializing the SoftDevice.
	static const app_usbd_config_t usbd_config = {};
	APP_ERROR_CHECK(app_usbd_init(&usbd_config));
	app_usbd_class_inst_t const *class_inst_generic = app_usbd_hid_generic_class_inst_get(&usb_hid);
	APP_ERROR_CHECK(app_usbd_class_append(class_inst_generic));
	app_usbd_enable();
	app_usbd_start();

	// Use board support package for LEDs.
	bsp_board_init(BSP_INIT_LEDS);

	// Initialize GPIO for dongle button. When button is pressed, delete all
	// peers to reestablish bond with the controller.
	gpio_init();

	// Enable SoftDevice.
	APP_ERROR_CHECK(nrf_sdh_enable_request());

	// Enable DC/DC regular to reduce energy consumption.
	sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE);

	// Enable timer. Even if not used directly, the timer is necessary for
	// Bluetooth LE.
	APP_ERROR_CHECK(app_timer_init());

	// Initialize Bluetooth LE.
	ble_init();
	gatt_init();
	peer_manager_init();

	// Initialize service discovery module.
    ble_db_discovery_init_t db_init = {
    	.evt_handler  = db_disc_handler,
    	.p_gatt_queue = &ble_gatt_queue,
	};
	APP_ERROR_CHECK(ble_db_discovery_init(&db_init));
	APP_ERROR_CHECK(ble_db_discovery_evt_register(&hids_uuid));

	// Initialize scanner and wait a bit before scanning. This is especially
	// important if the central needs to bond with the peripheral since we must
	// output the bonding passkey and will be unable do that before USB has
	// fully initialized. Ideally we would use a USB event though there doesn't
	// seem to be a suitable one for this purpose.
	scan_init();
	APP_TIMER_DEF(scan_timer);
	app_timer_create(&scan_timer, APP_TIMER_MODE_SINGLE_SHOT, scan_timer_handler);
	app_timer_start(scan_timer, APP_TIMER_TICKS(1000), NULL);

	// Create timer so we can output the passkey characters one at a time. The
	// event handler does not handle the output itself since it appears to have
	// no effect when done directly from the event handler.
	APP_TIMER_DEF(passkey_timer);
	app_timer_create(&passkey_timer, APP_TIMER_MODE_SINGLE_SHOT, passkey_timer_handler);

	while (true) {
		// Send queued message if nothing is already in progress.
		if (queue_count > 0 && !queue_pending) {
			uint8_t i = queue_i - queue_count;
			uint8_t *data = queue_data[i];
			uint8_t len = queue_lens[i];
			APP_ERROR_CHECK(app_usbd_hid_generic_in_report_set(&usb_hid, data, len));
			queue_pending = true;
			queue_count--;
		}

		// Handle USB and LESC events.
		while (app_usbd_event_queue_process());
		APP_ERROR_CHECK(nrf_ble_lesc_request_handler());

		// Handle passkey if one is waiting to be printed. The 6 digits should
		// be followed by a space for clarity. We can press it and release it
		// right away but handling multiple characters at once is problematic.
		// There are queuing limits and consecutive numbers that are identical
		// will be treated as one.
		if (passkey_output) {
			if (passkey_i == 6) {
				passkey_output_char(44);
			} else {
				uint8_t c = 30 + (passkey[passkey_i] - '0' + 9) % 10;
				passkey_output_char(c);
			}
			passkey_output_char(0);

			// Set timer if there are pending characters.
			passkey_i++;
			passkey_output = false;
			if (passkey_i < 7) {
				app_timer_start(passkey_timer, APP_TIMER_TICKS(100), NULL);
			}
		}

		while (NRF_LOG_PROCESS());
		sd_app_evt_wait();
	}
}
