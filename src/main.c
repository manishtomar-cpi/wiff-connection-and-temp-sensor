/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 * @brief WiFi station sample
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sta, CONFIG_LOG_DEFAULT_LEVEL);

#include <nrfx_clock.h>
#include <zephyr/kernel.h>
//for sensor
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

//for firebase 
#include <zephyr/net/socket.h>


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <zephyr/init.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/drivers/gpio.h>

#include <net/wifi_mgmt_ext.h>
#include <net/wifi_ready.h>

#include <qspi_if.h>

#include "net_private.h"

#define WIFI_SHELL_MODULE "wifi"

#define WIFI_SHELL_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT |		\
				NET_EVENT_WIFI_DISCONNECT_RESULT)

#define MAX_SSID_LEN        32
#define STATUS_POLLING_MS   300

/* 1000 msec = 1 sec */
#define LED_SLEEP_TIME_MS   100

/* The devicetree node identifier for the "led0" alias. */
#define LED0_NODE DT_ALIAS(led0)
/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static struct net_mgmt_event_callback wifi_shell_mgmt_cb;
static struct net_mgmt_event_callback net_shell_mgmt_cb;

#ifdef CONFIG_WIFI_READY_LIB
static K_SEM_DEFINE(wifi_ready_state_changed_sem, 0, 1);
static bool wifi_ready_status;
#endif /* CONFIG_WIFI_READY_LIB */


//PPD for sensor 
#define TMP117_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(ti_tmp116)


//for sensor 
void i2c_scan(const struct device *i2c_dev) {
    uint8_t i2c_addr;
    LOG_INF("Starting I2C scan...");

    for (i2c_addr = 0x03; i2c_addr <= 0x77; i2c_addr++) {
        if (i2c_write(i2c_dev, NULL, 0, i2c_addr) == 0) {
            LOG_INF("I2C device found at address 0x%02X", i2c_addr);
        }
    }

    LOG_INF("I2C scan complete.");
}

//firebase 
// Firebase server details
#define FIREBASE_HOST "nordic-data-default-rtdb.europe-west1.firebasedatabase.app"
#define FIREBASE_IP "34.107.226.223"
#define FIREBASE_PORT "80"  // HTTP
#define FIREBASE_PATH "/data.json"

#define REQUEST_TEMPLATE \
    "PUT " FIREBASE_PATH " HTTP/1.1\r\n" \
    "Host: " FIREBASE_HOST "\r\n" \
    "Content-Type: application/json\r\n" \
    "Content-Length: %d\r\n\r\n" \
    "%s"

static char response[1024];
// Function to send temperature data to Firebase using HTTP
int send_temperature_to_firebase(float temperature) {
    static struct addrinfo hints;
    struct addrinfo *res;
    int sock, ret;
    char *json_payload;
    char *request;

    LOG_INF("Preparing to send temperature data to Firebase");

    // Dynamically allocate memory for the JSON payload
    // Assuming the maximum payload size would be around 128 bytes
    json_payload = (char *)malloc(128 * sizeof(char));
    if (json_payload == NULL) {
        LOG_ERR("Failed to allocate memory for JSON payload");
        return -1;
    }

    // Create the JSON payload
    snprintf(json_payload, 128, "{\"temperature\": %.2f}", temperature);
    LOG_INF("JSON Payload: %s", json_payload);

    // Dynamically allocate memory for the HTTP request
    // Assuming the maximum request size would be around 512 bytes
    request = (char *)malloc(512 * sizeof(char));
    if (request == NULL) {
        LOG_ERR("Failed to allocate memory for HTTP request");
        free(json_payload); // Clean up previously allocated memory
        return -1;
    }

    // Create the HTTP request
    snprintf(request, 512, REQUEST_TEMPLATE, strlen(json_payload), json_payload);
    LOG_INF("HTTP Request: %s", request);

    // Initialize hints for the socket
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    // Resolve the Firebase IP address
    ret = getaddrinfo(FIREBASE_IP, FIREBASE_PORT, &hints, &res);
    if (ret != 0) {
        LOG_ERR("Unable to resolve Firebase address, quitting");
        free(json_payload);
        free(request);
        return -1;
    }

    // Create a non-TLS socket (for HTTP)
    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        LOG_ERR("Failed to create socket");
        freeaddrinfo(res);
        free(json_payload);
        free(request);
        return -1;
    }

    // Connect to the Firebase server
    ret = connect(sock, res->ai_addr, res->ai_addrlen);
    if (ret < 0) {
        LOG_ERR("Failed to connect to Firebase");
        close(sock);
        freeaddrinfo(res);
        free(json_payload);
        free(request);
        return -1;
    }

    // Send the HTTP request
    ret = send(sock, request, strlen(request), 0);
    if (ret < 0) {
        LOG_ERR("Failed to send request to Firebase");
        close(sock);
        freeaddrinfo(res);
        free(json_payload);
        free(request);
        return -1;
    }

    LOG_INF("Request sent to Firebase");

    // Receive the response from Firebase
    while (1) {
        int len = recv(sock, response, sizeof(response) - 1, 0);
        if (len < 0) {
            LOG_ERR("Error reading response");
            break;
        }
        if (len == 0) {
            break;
        }
        response[len] = 0;
        LOG_INF("Response: %s", response);
    }

    // Clean up
    close(sock);
    freeaddrinfo(res);
    free(json_payload);  // Free dynamically allocated memory
    free(request);       // Free dynamically allocated memory

    return 0;
}


static struct {
	const struct shell *sh;
	union {
		struct {
			uint8_t connected	: 1;
			uint8_t connect_result	: 1;
			uint8_t disconnect_requested	: 1;
			uint8_t _unused		: 5;
		};
		uint8_t all;
	};
} context;

void toggle_led(void)
{
	int ret;

	if (!device_is_ready(led.port)) {
		LOG_ERR("LED device is not ready");
		return;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Error %d: failed to configure LED pin", ret);
		return;
	}

	while (1) {
		if (context.connected) {
			gpio_pin_toggle_dt(&led);
			k_msleep(LED_SLEEP_TIME_MS);
		} else {
			gpio_pin_set_dt(&led, 0);
			k_msleep(LED_SLEEP_TIME_MS);
		}
	}
}

K_THREAD_DEFINE(led_thread_id, 1024, toggle_led, NULL, NULL, NULL,
		7, 0, 0);

static int cmd_wifi_status(void)
{
	struct net_if *iface = net_if_get_default();
	struct wifi_iface_status status = { 0 };

	if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status,
				sizeof(struct wifi_iface_status))) {
		LOG_INF("Status request failed");

		return -ENOEXEC;
	}

	LOG_INF("==================");
	LOG_INF("State: %s", wifi_state_txt(status.state));

	if (status.state >= WIFI_STATE_ASSOCIATED) {
		uint8_t mac_string_buf[sizeof("xx:xx:xx:xx:xx:xx")];

		LOG_INF("Interface Mode: %s",
		       wifi_mode_txt(status.iface_mode));
		LOG_INF("Link Mode: %s",
		       wifi_link_mode_txt(status.link_mode));
		LOG_INF("SSID: %.32s", status.ssid);
		LOG_INF("BSSID: %s",
		       net_sprint_ll_addr_buf(
				status.bssid, WIFI_MAC_ADDR_LEN,
				mac_string_buf, sizeof(mac_string_buf)));
		LOG_INF("Band: %s", wifi_band_txt(status.band));
		LOG_INF("Channel: %d", status.channel);
		LOG_INF("Security: %s", wifi_security_txt(status.security));
		LOG_INF("MFP: %s", wifi_mfp_txt(status.mfp));
		LOG_INF("RSSI: %d", status.rssi);
	}
	return 0;
}

static void handle_wifi_connect_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status =
		(const struct wifi_status *) cb->info;

	if (context.connected) {
		return;
	}

	if (status->status) {
		LOG_ERR("Connection failed (%d)", status->status);
	} else {
		LOG_INF("Connected");
		context.connected = true;
	}

	context.connect_result = true;
}

static void handle_wifi_disconnect_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status =
		(const struct wifi_status *) cb->info;

	if (!context.connected) {
		return;
	}

	if (context.disconnect_requested) {
		LOG_INF("Disconnection request %s (%d)",
			 status->status ? "failed" : "done",
					status->status);
		context.disconnect_requested = false;
	} else {
		LOG_INF("Received Disconnected");
		context.connected = false;
	}

	cmd_wifi_status();
}

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				     uint32_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		handle_wifi_connect_result(cb);
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		handle_wifi_disconnect_result(cb);
		break;
	default:
		break;
	}
}

static void print_dhcp_ip(struct net_mgmt_event_callback *cb)
{
	/* Get DHCP info from struct net_if_dhcpv4 and print */
	const struct net_if_dhcpv4 *dhcpv4 = cb->info;
	const struct in_addr *addr = &dhcpv4->requested_ip;
	char dhcp_info[128];

	net_addr_ntop(AF_INET, addr, dhcp_info, sizeof(dhcp_info));

	LOG_INF("DHCP IP address: %s", dhcp_info);
}
static void net_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				    uint32_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_IPV4_DHCP_BOUND:
		print_dhcp_ip(cb);
		break;
	default:
		break;
	}
}

static int wifi_connect(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	context.connected = false;
	context.connect_result = false;

	if (net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0)) {
		LOG_ERR("Connection request failed");

		return -ENOEXEC;
	}

	LOG_INF("Connection requested");

	return 0;
}

int bytes_from_str(const char *str, uint8_t *bytes, size_t bytes_len)
{
	size_t i;
	char byte_str[3];

	if (strlen(str) != bytes_len * 2) {
		LOG_ERR("Invalid string length: %zu (expected: %d)\n",
			strlen(str), bytes_len * 2);
		return -EINVAL;
	}

	for (i = 0; i < bytes_len; i++) {
		memcpy(byte_str, str + i * 2, 2);
		byte_str[2] = '\0';
		bytes[i] = strtol(byte_str, NULL, 16);
	}

	return 0;
}

int start_app(void)
{
#if defined(CONFIG_BOARD_NRF7002DK_NRF7001_NRF5340_CPUAPP) || \
	defined(CONFIG_BOARD_NRF7002DK_NRF5340_CPUAPP)
	if (strlen(CONFIG_NRF700X_QSPI_ENCRYPTION_KEY)) {
		int ret;
		char key[QSPI_KEY_LEN_BYTES];

		ret = bytes_from_str(CONFIG_NRF700X_QSPI_ENCRYPTION_KEY, key, sizeof(key));
		if (ret) {
			LOG_ERR("Failed to parse encryption key: %d\n", ret);
			return 0;
		}

		LOG_DBG("QSPI Encryption key: ");
		for (int i = 0; i < QSPI_KEY_LEN_BYTES; i++) {
			LOG_DBG("%02x", key[i]);
		}
		LOG_DBG("\n");

		ret = qspi_enable_encryption(key);
		if (ret) {
			LOG_ERR("Failed to enable encryption: %d\n", ret);
			return 0;
		}
		LOG_INF("QSPI Encryption enabled");
	} else {
		LOG_INF("QSPI Encryption disabled");
	}
#endif /* CONFIG_BOARD_NRF700XDK_NRF5340 */

	LOG_INF("Static IP address (overridable): %s/%s -> %s",
		CONFIG_NET_CONFIG_MY_IPV4_ADDR,
		CONFIG_NET_CONFIG_MY_IPV4_NETMASK,
		CONFIG_NET_CONFIG_MY_IPV4_GW);

	while (1) {
#ifdef CONFIG_WIFI_READY_LIB
		int ret;

		LOG_INF("Waiting for Wi-Fi to be ready");
		ret = k_sem_take(&wifi_ready_state_changed_sem, K_FOREVER);
		if (ret) {
			LOG_ERR("Failed to take semaphore: %d", ret);
			return ret;
		}

check_wifi_ready:
		if (!wifi_ready_status) {
			LOG_INF("Wi-Fi is not ready");
			/* Perform any cleanup and stop using Wi-Fi and wait for
			 * Wi-Fi to be ready
			 */
			continue;
		}
#endif /* CONFIG_WIFI_READY_LIB */
		wifi_connect();

		while (!context.connect_result) {
			cmd_wifi_status();
			k_sleep(K_MSEC(STATUS_POLLING_MS));
		}

		if (context.connected) {
			cmd_wifi_status();
#ifdef CONFIG_WIFI_READY_LIB
			ret = k_sem_take(&wifi_ready_state_changed_sem, K_FOREVER);
			if (ret) {
				LOG_ERR("Failed to take semaphore: %d", ret);
				return ret;
			}
			goto check_wifi_ready;
#else
			k_sleep(K_FOREVER);
#endif /* CONFIG_WIFI_READY_LIB */
		}
	}

	return 0;
}

#ifdef CONFIG_WIFI_READY_LIB
void start_wifi_thread(void);
#define THREAD_PRIORITY K_PRIO_COOP(CONFIG_NUM_COOP_PRIORITIES - 1)
K_THREAD_DEFINE(start_wifi_thread_id, CONFIG_STA_SAMPLE_START_WIFI_THREAD_STACK_SIZE,
		start_wifi_thread, NULL, NULL, NULL,
		THREAD_PRIORITY, 0, -1);

void start_wifi_thread(void)
{
	start_app();
}

void wifi_ready_cb(bool wifi_ready)
{
	LOG_DBG("Is Wi-Fi ready?: %s", wifi_ready ? "yes" : "no");
	wifi_ready_status = wifi_ready;
	k_sem_give(&wifi_ready_state_changed_sem);
}
#endif /* CONFIG_WIFI_READY_LIB */

void net_mgmt_callback_init(void)
{
	memset(&context, 0, sizeof(context));

	net_mgmt_init_event_callback(&wifi_shell_mgmt_cb,
				     wifi_mgmt_event_handler,
				     WIFI_SHELL_MGMT_EVENTS);

	net_mgmt_add_event_callback(&wifi_shell_mgmt_cb);

	net_mgmt_init_event_callback(&net_shell_mgmt_cb,
				     net_mgmt_event_handler,
				     NET_EVENT_IPV4_DHCP_BOUND);

	net_mgmt_add_event_callback(&net_shell_mgmt_cb);

	LOG_INF("Starting %s with CPU frequency: %d MHz", CONFIG_BOARD, SystemCoreClock/MHZ(1));
	k_sleep(K_SECONDS(1));
}

#ifdef CONFIG_WIFI_READY_LIB
static int register_wifi_ready(void)
{
	int ret = 0;
	wifi_ready_callback_t cb;
	struct net_if *iface = net_if_get_first_wifi();

	if (!iface) {
		LOG_ERR("Failed to get Wi-Fi interface");
		return -1;
	}

	cb.wifi_ready_cb = wifi_ready_cb;

	LOG_DBG("Registering Wi-Fi ready callbacks");
	ret = register_wifi_ready_callback(cb, iface);
	if (ret) {
		LOG_ERR("Failed to register Wi-Fi ready callbacks %s", strerror(ret));
		return ret;
	}

	return ret;
}
#endif /* CONFIG_WIFI_READY_LIB */

int main(void)
{
    // Sensor initialization
    const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));
    if (!device_is_ready(i2c_dev)) {
        LOG_ERR("I2C device is not ready");
        return -1;
    }
    
    // Perform an I2C scan to detect connected devices
    i2c_scan(i2c_dev);

    // Get the TMP117 device using the device tree node
    const struct device *const dev = DEVICE_DT_GET(TMP117_NODE);
    if (!device_is_ready(dev)) {
        LOG_ERR("TMP117 device is not ready");
        return -1;
    }

    struct sensor_value temp;

    // Wi-Fi connection
    int ret = 0;
    net_mgmt_callback_init();

#ifdef CONFIG_WIFI_READY_LIB
    ret = register_wifi_ready();
    if (ret) {
        return ret;
    }
    k_thread_start(start_wifi_thread_id);
#else
    start_app();
#endif /* CONFIG_WIFI_READY_LIB */

    // Poll for Wi-Fi connection status
    while (!context.connected) {
        LOG_INF("Waiting for Wi-Fi to connect...");
        k_sleep(K_MSEC(500));  // Poll every 500 ms
    }

    LOG_INF("Wi-Fi connected, proceeding to fetch temperature data.");

    // Now that Wi-Fi is connected, fetch sensor data
    if (sensor_sample_fetch(dev) < 0) {
        LOG_ERR("Sensor sample update error");
        return -1;
    }

    sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
    float temperature = temp.val1 + (temp.val2 / 1000000.0);
    LOG_INF("Temperature: %d.%06d C", temp.val1, temp.val2);

    // Send the temperature data to Firebase
    send_temperature_to_firebase(temperature);

    return ret;
}
