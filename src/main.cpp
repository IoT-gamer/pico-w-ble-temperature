#include <Arduino.h>
#include <BTstackLib.h>
#include <hardware/adc.h>

// Direct BTstack includes
#include "ble/att_server.h"
#include "bluetooth.h"
#include "btstack_event.h"

// Temperature service UUID (custom)
const char* TEMP_SERVICE_UUID = "181A";  // Environmental Sensing service
const char* TEMP_CHAR_UUID = "2A6E";     // Temperature characteristic

static uint16_t temp_char_handle = 0;
static float last_temp = 0.0f;
static uint32_t last_notify_time = 0;
const uint32_t NOTIFY_INTERVAL = 1000;  // Notify every 1 second
static bool client_subscribed = false;
static hci_con_handle_t con_handle = HCI_CON_HANDLE_INVALID;
static bool notification_pending = false;
static float pending_temperature = 0.0f;

// Read temperature from Pico-W internal temperature sensor
float readTemperature() {
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);  // Temperature sensor is on input 4
    
    uint16_t raw = adc_read();
    float voltage = (raw * 3.3f) / 4096.0f;
    float temperature = 27.0f - (voltage - 0.706f) / 0.001721f;
    
    return temperature;
}

// Send temperature notification using direct BTstack calls
bool notify_client_temperature(float temperature) {
    if (!client_subscribed || con_handle == HCI_CON_HANDLE_INVALID) {
        return false;
    }

    if (att_server_can_send_packet_now(con_handle)) {
        // Convert temperature to 16-bit integer (multiply by 100 to preserve 2 decimal places)
        int16_t temp_int = (int16_t)(temperature * 100);
        uint8_t value[2];
        value[0] = temp_int & 0xFF;
        value[1] = (temp_int >> 8) & 0xFF;
        
        // Send notification using direct BTstack call
        if (att_server_notify(con_handle, temp_char_handle, value, sizeof(value)) == 0) {
            Serial.printf("Notification sent: %.2f°C\n", temperature);
            notification_pending = false;
            return true;
        }
    } else {
        Serial.println("Client busy. Requesting notification permission...");
        pending_temperature = temperature;
        notification_pending = true;
        att_server_request_can_send_now_event(con_handle);
    }
    return false;
}

// Global callback registration and temperature queue
static btstack_packet_callback_registration_t hci_event_callback_registration;

// BTstack packet handler
static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (packet_type != HCI_EVENT_PACKET) return;
    
    uint8_t event = packet[0];
    if (event == ATT_EVENT_CAN_SEND_NOW) {
        if (notification_pending) {
            notify_client_temperature(pending_temperature);
        }
    }
}

// Callback for BLE device connections
void deviceConnectedCallback(BLEStatus status, BLEDevice *device) {
    switch (status) {
        case BLE_STATUS_OK:
            con_handle = device->getHandle();
            Serial.println("Device connected!");
            break;
        default:
            break;
    }
}

// Callback for BLE device disconnections
void deviceDisconnectedCallback(BLEDevice *device) {
    Serial.println("Device disconnected!");
    con_handle = HCI_CON_HANDLE_INVALID;
    client_subscribed = false;
    notification_pending = false;
}

// Callback for characteristic reads
uint16_t gattReadCallback(uint16_t value_handle, uint8_t* buffer, uint16_t buffer_size) {
    if (buffer) {
        float temp = readTemperature();
        // Convert temperature to 16-bit integer (multiply by 100 to preserve 2 decimal places)
        int16_t temp_int = (int16_t)(temp * 100);
        memcpy(buffer, &temp_int, sizeof(int16_t));
        Serial.printf("Temperature read: %.2f°C\n", temp);
    }
    return sizeof(int16_t);
}

// Callback for characteristic writes (including CCC descriptor writes)
int gattWriteCallback(uint16_t value_handle, uint8_t *buffer, uint16_t buffer_size) {
    // Check if this is a write to the CCC descriptor (notifications enable/disable)
    if (buffer_size == 2) {
        uint16_t value = (buffer[1] << 8) | buffer[0];
        if (value == 0x0001) {
            client_subscribed = true;
            Serial.println("Notifications enabled by client");
        } else if (value == 0x0000) {
            client_subscribed = false;
            notification_pending = false;
            Serial.println("Notifications disabled by client");
        }
    }
    return 0;
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10); // Wait for Serial to be ready
    
    Serial.println("Pico-W BLE Temperature Sensor");
    
    // Set up BLE callbacks
    BTstack.setBLEDeviceConnectedCallback(deviceConnectedCallback);
    BTstack.setBLEDeviceDisconnectedCallback(deviceDisconnectedCallback);
    BTstack.setGATTCharacteristicRead(gattReadCallback);
    BTstack.setGATTCharacteristicWrite(gattWriteCallback);
    // Register for HCI events
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    
    // Add Environmental Sensing service and temperature characteristic
    BTstack.addGATTService(new UUID(TEMP_SERVICE_UUID));
    temp_char_handle = BTstack.addGATTCharacteristicDynamic(
        new UUID(TEMP_CHAR_UUID),
        ATT_PROPERTY_READ | ATT_PROPERTY_NOTIFY,
        0
    );
    
    // Start BLE
    BTstack.setup("Pico-W Temp");
    BTstack.startAdvertising();
    
    Serial.println("BLE Temperature Sensor started");
}

void loop() {
    BTstack.loop();
    
    // Read and update temperature periodically
    uint32_t current_time = millis();
    if (current_time - last_notify_time >= NOTIFY_INTERVAL) {
        float temp = readTemperature();
        
        // Only notify if temperature has changed and no notification is pending
        if (!notification_pending && abs(temp - last_temp) >= 0.1f) {  // 0.1°C threshold
            if (notify_client_temperature(temp)) {
                last_temp = temp;
            }
        }
        
        last_notify_time = current_time;
    }
}