/* Sonoff TH10/16
 *
 * In order to flash the Sonoff TH you will have to
 * have a 3,3v (logic level) FTDI adapter.
 *
 * To flash this example connect 3,3v, TX, RX, GND
 * in this order, beginning in the (VCC) pin header.
 *
 * Next hold down the button and connect the FTDI adapter
 * to your computer. The sonoff is now in flash mode and
 * you can flash the custom firmware.
 *
 * WARNING: Do not connect the sonoff to AC while it's
 * connected to the FTDI adapter! This may fry your
 * computer and sonoff.
 *
 */

#include <stdio.h>
#include <esp/uart.h>
#include <esplibs/libmain.h>
#include <esp8266.h>
#include <FreeRTOS.h>
#include <task.h>
#include <etstimer.h>
#include <espressif/esp_wifi.h>
#include <espressif/esp_system.h>
#include <espressif/esp_sta.h>

#include <homekit/homekit.h>
#include <homekit/characteristics.h>
#include <wifi_config.h>

#include <led_codes.h>

#include <dht/dht.h>

#define BUTTON_GPIO     0
#define LED_GPIO        13
#define RELAY_GPIO      12
#define SENSOR_GPIO     14

#define POLL_PERIOD_A     10000
#define POLL_PERIOD_B     20000


void relay_write(bool on) {
    gpio_write(RELAY_GPIO, on ? 1 : 0);
}

void led_write(bool on) {
    gpio_write(LED_GPIO, on ? 0 : 1);
}

void update_state();

void on_update(homekit_characteristic_t *ch, homekit_value_t value, void *context) {
    update_state();
}

void identify_task(void *_args) {
    led_code(LED_GPIO, IDENTIFY_ACCESSORY);
    vTaskDelete(NULL);
}

void wifi_connected_task(void *_args) {
    led_code(LED_GPIO, WIFI_CONNECTED);
    vTaskDelete(NULL);
}

void identify(homekit_value_t _value) {
    printf(">>> Identifying\n");
    xTaskCreate(identify_task, "Identify", 128, NULL, 2, NULL);
}

homekit_characteristic_t current_temperature = HOMEKIT_CHARACTERISTIC_(CURRENT_TEMPERATURE, 17);
homekit_characteristic_t target_temperature  = HOMEKIT_CHARACTERISTIC_(TARGET_TEMPERATURE, 22, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(on_update));
homekit_characteristic_t units = HOMEKIT_CHARACTERISTIC_(TEMPERATURE_DISPLAY_UNITS, 0);
homekit_characteristic_t current_state = HOMEKIT_CHARACTERISTIC_(CURRENT_HEATING_COOLING_STATE, 0);
homekit_characteristic_t target_state = HOMEKIT_CHARACTERISTIC_(TARGET_HEATING_COOLING_STATE, 0, .callback=HOMEKIT_CHARACTERISTIC_CALLBACK(on_update));
homekit_characteristic_t current_humidity = HOMEKIT_CHARACTERISTIC_(CURRENT_RELATIVE_HUMIDITY, 50);

void update_state() {
    uint8_t state = target_state.value.int_value;
    if (state == 1 && current_temperature.value.float_value < target_temperature.value.float_value) {
        if (current_state.value.int_value != 1) {
            current_state.value = HOMEKIT_UINT8(1);
            homekit_characteristic_notify(&current_state, current_state.value);
            
            relay_write(true);
        }
    } else if (state == 2 && current_temperature.value.float_value > target_temperature.value.float_value) {
        if (current_state.value.int_value != 2) {
            current_state.value = HOMEKIT_UINT8(2);
            homekit_characteristic_notify(&current_state, current_state.value);
            
            relay_write(true);
        }
    } else if (current_state.value.int_value != 0) {
        current_state.value = HOMEKIT_UINT8(0);
        homekit_characteristic_notify(&current_state, current_state.value);
            
        relay_write(false);
    }
}

void temperature_sensor_task(void *_args) {
    gpio_enable(LED_GPIO, GPIO_OUTPUT);
    led_write(false);
    
    gpio_set_pullup(SENSOR_GPIO, false, false);
    
    gpio_enable(RELAY_GPIO, GPIO_OUTPUT);
    relay_write(false);
    
    float humidity_value, temperature_value;
    while (1) {
        vTaskDelay(POLL_PERIOD_A / portTICK_PERIOD_MS);
        
        if (dht_read_float_data(DHT_TYPE_DHT22, SENSOR_GPIO, &humidity_value, &temperature_value)) {
            printf(">>> Sensor: temperature %g, humidity %g\n", temperature_value, humidity_value);
            current_temperature.value = HOMEKIT_FLOAT(temperature_value);
            current_humidity.value = HOMEKIT_FLOAT(humidity_value);
            
            homekit_characteristic_notify(&current_temperature, current_temperature.value);
            homekit_characteristic_notify(&current_humidity, current_humidity.value);
            
            update_state();
            
            vTaskDelay(POLL_PERIOD_B / portTICK_PERIOD_MS);
        } else {
            printf(">>> Sensor: ERROR\n");
            
            if (current_state.value.int_value != 0) {
                current_state.value = HOMEKIT_UINT8(0);
                homekit_characteristic_notify(&current_state, current_state.value);
                
                relay_write(false);
            }
            
            led_code(LED_GPIO, SENSOR_ERROR);
        }
    }
}

void thermostat_init() {
    xTaskCreate(temperature_sensor_task, "Thermostat", 256, NULL, 2, NULL);
}

homekit_characteristic_t name = HOMEKIT_CHARACTERISTIC_(NAME, "Sonoff Thermostat");
homekit_characteristic_t serial = HOMEKIT_CHARACTERISTIC_(SERIAL_NUMBER, "SonoffTH N/A");

homekit_accessory_t *accessories[] = {
    HOMEKIT_ACCESSORY(.id=1, .category=homekit_accessory_category_thermostat, .services=(homekit_service_t*[]) {
        HOMEKIT_SERVICE(ACCESSORY_INFORMATION, .characteristics=(homekit_characteristic_t*[]) {
            &name,
            HOMEKIT_CHARACTERISTIC(MANUFACTURER, "iTEAD"),
            &serial,
            HOMEKIT_CHARACTERISTIC(MODEL, "Sonoff TH"),
            HOMEKIT_CHARACTERISTIC(FIRMWARE_REVISION, "1.0"),
            HOMEKIT_CHARACTERISTIC(IDENTIFY, identify),
            NULL
        }),
        HOMEKIT_SERVICE(THERMOSTAT, .primary=true, .characteristics=(homekit_characteristic_t*[]) {
            HOMEKIT_CHARACTERISTIC(NAME, "Sonoff Thermostat"),
            &current_temperature,
            &target_temperature,
            &current_state,
            &target_state,
            &units,
            &current_humidity,
            NULL
        }),
        NULL
    }),
    NULL
};

homekit_server_config_t config = {
    .accessories = accessories,
    .password = "021-82-017"
};

void create_accessory_name() {
    // Get MAC address
    uint8_t macaddr[6];
    sdk_wifi_get_macaddr(STATION_IF, macaddr);
    
    // Use MAC address as part of accesory name and serial number
    uint8_t name_len = snprintf(NULL, 0, "SonoffTH %02X%02X%02X", macaddr[3], macaddr[4], macaddr[5]);
    char *name_value = malloc(name_len+1);
    snprintf(name_value, name_len+1, "SonoffTH %02X%02X%02X", macaddr[3], macaddr[4], macaddr[5]);
    
    name.value = HOMEKIT_STRING(name_value);
    serial.value = HOMEKIT_STRING(name_value);
}

void on_wifi_ready() {
    xTaskCreate(wifi_connected_task, "Wifi connected", 128, NULL, 1, NULL);
    
    create_accessory_name();
        
    // Start HomeKit
    homekit_server_init(&config);
}

void user_init(void) {
    uart_set_baud(0, 115200);
    
    wifi_config_init("SonoffTH", NULL, on_wifi_ready);
    
    thermostat_init();
}