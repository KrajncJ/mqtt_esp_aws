
#include "espressif/esp_common.h"
#include "esp/uart.h"
#include <string.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <espressif/esp_sta.h>
#include <espressif/esp_wifi.h>
#include <paho_mqtt_c/MQTTESP8266.h>
#include <paho_mqtt_c/MQTTClient.h>
#include "ssl_connection.h"
#include "bmp280/bmp280.h"
#include "i2c/i2c.h"






// Put this values into wifi config file
#define WIFI_NAME "***"
#define WIFI_PASS "***"

// MQTT config // 
#define mqtt_server "a2zttsshc1wnh8.iot.eu-west-1.amazonaws.com"
#define MQTT_PORT 8883
extern char *ca_cert, *client_cert, *client_key; // Certificates for amazon SSL connection

// I2C config constants for I2C init (for sensor captuiring)
const uint8_t i2c_bus = 0;
const uint8_t scl_pin = 14;
const uint8_t sda_pin = 12;

// Mesure after each interval
const int MESURE_INTERVAL_MS = 15000; // Change this value if needed
static QueueHandle_t xQueue; // Queue for measures

// Device MAC address for identification
char device_mac[13];
char mqtt_publish_topic[100];

// SSL variables
static int ssl_invalid;
static SSLConnection *ssl_connection;

// Measures struct
struct Measure {
   float  temperature;
   float  air_pressure;
};


static int is_wifi_connected() {
	if (sdk_wifi_station_get_connect_status() == STATION_GOT_IP ) {
		return 1;
	}
	printf("is_wifi_connected: Wifi not connected! \n");
	return 0;
}


static const char* get_station_mac(void)
{
    static char my_id[32];
    static bool my_id_done = false;
    uint8_t hwaddr[6];
    if (my_id_done)
        return my_id;
    if (!sdk_wifi_get_macaddr(STATION_IF, (uint8_t*)hwaddr))
        return NULL;
    snprintf(my_id, sizeof(my_id), "%02x%02x%02x%02x%02x%02x", MAC2STR(hwaddr));
    my_id_done = true;
    return my_id;
}

static int mqtt_ssl_read(mqtt_network_t * n, unsigned char* buffer, int len,
        int timeout_ms) {
    int r = ssl_read(ssl_connection, buffer, len, timeout_ms);
    if (r <= 0
            && (r != MBEDTLS_ERR_SSL_WANT_READ
                    && r != MBEDTLS_ERR_SSL_WANT_WRITE
                    && r != MBEDTLS_ERR_SSL_TIMEOUT)) {
        printf("%s: TLS read error (%d), resetting\n\r", __func__, r);
        ssl_invalid = 1;
    };
    return r;
}

static int mqtt_ssl_write(mqtt_network_t* n, unsigned char* buffer, int len,
        int timeout_ms) {
    int r = ssl_write(ssl_connection, buffer, len, timeout_ms);
    if (r <= 0
            && (r != MBEDTLS_ERR_SSL_WANT_READ
                    && r != MBEDTLS_ERR_SSL_WANT_WRITE)) {
        printf("%s: TLS write error (%d), resetting\n\r", __func__, r);
        ssl_invalid = 1;
    }
    return r;
}

static void mqtt_task(void *pvParameters) {
    
	int ret = 0;
	
    struct mqtt_network network;
    uint8_t mqtt_buf[100];
    uint8_t mqtt_readbuf[100];
    
    mqtt_client_t client = mqtt_client_default;
    mqtt_packet_connect_data_t data = mqtt_packet_connect_data_initializer;
    ssl_connection = (SSLConnection *) malloc(sizeof(SSLConnection));
    
    
    while (1) {
    	printf("Is wifi alive (MQTT task) \n");
    	
    	int wifi_ok = is_wifi_connected();	
        if (!wifi_ok) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        printf("%s: started\n\r", __func__);
        ssl_invalid = 0;
        ssl_init(ssl_connection);
        ssl_connection->ca_cert_str = ca_cert;
        ssl_connection->client_cert_str = client_cert;
        ssl_connection->client_key_str = client_key;

        mqtt_network_new(&network);
        network.mqttread = mqtt_ssl_read;
        network.mqttwrite = mqtt_ssl_write;

        printf("%s: connecting to MQTT server %s ... ", __func__,
                mqtt_server);
        ret = ssl_connect(ssl_connection, mqtt_server, MQTT_PORT);

        if (ret) {
            printf("error: %d\n\r", ret);
            ssl_destroy(ssl_connection);
            continue;
        }
        printf("done\n\r");
        mqtt_client_new(&client, &network, 5000, mqtt_buf, 100, mqtt_readbuf,
                100);

        data.willFlag = 0;
        data.MQTTVersion = 4;
        data.cleansession = 1;
        data.clientID.cstring = device_mac;
        data.username.cstring = NULL;
        data.password.cstring = NULL;
        data.keepAliveInterval = 1000;
        printf("Send MQTT connect ... ");
        ret = mqtt_connect(&client, &data);
        if (ret) {
            printf("Error: %d \n", ret);
            ssl_destroy(ssl_connection);
            continue;
        }
        printf("Done \n");
  

        while (wifi_ok && !ssl_invalid) {
            char msg[64];
            struct Measure receivedMeasure;
            // Wait here for new measures...
            while (xQueueReceive(xQueue, &receivedMeasure , 0) == pdTRUE) {
            	
            	printf("Queue detected new item... \n");
            	
            	// Build message
                snprintf(msg, sizeof(msg), "{ \"temperature\": \"%.2f\", \"air_pressure\": \"%.2f\" }",
                		receivedMeasure.temperature, receivedMeasure.air_pressure);
      
                mqtt_message_t message;
                message.payload = msg;
                message.payloadlen = strlen(msg);
                message.dup = 0;
                message.qos = MQTT_QOS1;
                message.retained = 0;
              
                printf("Publishing: %s\r\n", msg);
                
                ret = mqtt_publish(&client, mqtt_publish_topic, &message);
                
                if (ret != MQTT_SUCCESS) {
                    printf("Error publishing message: %d \n", ret);
                    break;
                } else {
                	printf("Successfully published \n");
                }
            }

            ret = mqtt_yield(&client, 1000);
            if (ret == MQTT_DISCONNECTED)
                break;
        }
        printf("Connection dropped, request restart\n\r");
        ssl_destroy(ssl_connection);
    }
}


static void bmp280_task_forced(void *pvParameters)
{	
	
	printf("bmp280_task_forced start... \n");
	
    float pressure, temperature;
    
    // Constructs
    bmp280_params_t  params;
    bmp280_t bmp280_dev;

    // Initialize bmp sensor
    bmp280_init_default_params(&params);
    params.mode = BMP280_MODE_FORCED;    
    bmp280_dev.i2c_dev.bus = i2c_bus;
    bmp280_dev.i2c_dev.addr = BMP280_I2C_ADDRESS_0;
    
    // Main loop with sensor init
    while (1) {
        while (!bmp280_init(&bmp280_dev, &params)) {
            printf("BMP280 sensor initialization failed\n");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        
        // Measure loop
        while(1) {
        	
        	printf("Messuring... \n");
        	vTaskDelay(3000 / portTICK_PERIOD_MS);
        	

        	// Force measure
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            if (!bmp280_force_measurement(&bmp280_dev)) {
                printf("Force measure init failed \n");
                break;
            }
            
            // Wait for measurement
            while (bmp280_is_measuring(&bmp280_dev)) {};

            if (!bmp280_read_float(&bmp280_dev, &temperature, &pressure, NULL)) {
                printf("Reading temp failed \n");
                break;
            }
            
            printf("Pushing temp to queue.. \n");
            
            struct Measure measure1;       
            measure1.temperature = temperature;
            measure1.air_pressure = pressure;
            
            
            xQueueSend(xQueue, &measure1, 0 );
            
     
            // Delay measures
			vTaskDelay(MESURE_INTERVAL_MS / portTICK_PERIOD_MS);
   
        }
    }
}



void initialize_wifi_connection() {

    printf("Establishing WI-FI connection ...");

    struct sdk_station_config config = { .ssid = WIFI_NAME, .password =
    		WIFI_PASS, };
    sdk_wifi_station_set_auto_connect(1);
	sdk_wifi_set_opmode(STATION_MODE);
	sdk_wifi_station_set_config(&config);
	sdk_wifi_station_connect();
	
	printf("done \n");
    
}

void user_init(void) {
    uart_set_baud(0, 115200);
     
    // Connect module to WI-FI
    initialize_wifi_connection();
    
    // Initialize device mac
    snprintf(device_mac, sizeof(device_mac)-1, "%s", get_station_mac());
    
    // Init publish topic
    snprintf(mqtt_publish_topic, sizeof(mqtt_publish_topic), "sensor/outside/%s/measures",
                		device_mac);
                
    printf("Device MAC: ");
    printf(device_mac);
    printf("\n");
    printf("Publish topic: ");
    printf(mqtt_publish_topic);
    printf("\n");
    
    // INIT I2C for sensors
    i2c_init(i2c_bus, scl_pin, sda_pin, I2C_FREQ_400K);
    
    // Create queue to hold sensor data
    xQueue = xQueueCreate(3, sizeof(struct Measure));

    
    xTaskCreate(&bmp280_task_forced, "bmp280_task_forced", 1048, NULL, 2, NULL);
    xTaskCreate(&mqtt_task, "mqtt_task", 2048, NULL, 2, NULL);

}
