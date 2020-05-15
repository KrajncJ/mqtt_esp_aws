
#include "espressif/esp_common.h"
#include <esp8266.h>
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
#include <ssid_config.h>
#include <httpd/httpd.h>
#include <stdio.h>
#include <stdlib.h>

// Put this values into wifi config file
#define WIFI_NAME "SteinsGate;"
#define WIFI_PASS "Danganronpa"

// MQTT config // 
#define mqtt_server "a2zttsshc1wnh8.iot.eu-west-1.amazonaws.com"
#define MQTT_PORT 8883

#define LED_PIN 2
extern char *ca_cert, *client_cert, *client_key; // Certificates for amazon SSL connection

// I2C config constants for I2C init (for sensor captuiring)
const uint8_t i2c_bus = 0;
const uint8_t scl_pin = 14;
const uint8_t sda_pin = 12;

// Mesure after each interval
const int MESURE_INTERVAL_MS = 20000; // Change this value if needed
static QueueHandle_t xQueue; // Queue for measures
static bool MQTT_connection_established = false;
static bool bmp280_init_successful = false;

// Device MAC address for identification
char device_mac[13];
char mqtt_publish_topic[100];
char mqtt_subscribe_topic[100];

// SSL variables
static int ssl_invalid;
static SSLConnection *ssl_connection;

// Measures struct
struct Measure {
   float  temperature;
   float  air_pressure;
};

struct Device {
   bool initialized;
   char device_mac[13];
   char last_measure[80];
   float temperature;
   float air_pressure;
};

static struct Device ConnectedDevices[2];

enum {
    SSI_UPTIME,
    SSI_FREE_HEAP,
    SSI_LED_STATE
};

int32_t ssi_handler(int32_t iIndex, char *pcInsert, int32_t iInsertLen)
{
    switch (iIndex) {
        case SSI_UPTIME:
            snprintf(pcInsert, iInsertLen, "%d",
                    xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
            break;
        case SSI_FREE_HEAP:
            snprintf(pcInsert, iInsertLen, "%d", (int) xPortGetFreeHeapSize());
            break;
        case SSI_LED_STATE:
            snprintf(pcInsert, iInsertLen, (GPIO.OUT & BIT(LED_PIN)) ? "Off" : "On");
            break;
        default:
            snprintf(pcInsert, iInsertLen, "N/A");
            break;
    }

    /* Tell the server how many characters to insert */
    return (strlen(pcInsert));
}

char *gpio_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[])
{
    for (int i = 0; i < iNumParams; i++) {
        if (strcmp(pcParam[i], "on") == 0) {
            uint8_t gpio_num = atoi(pcValue[i]);
            gpio_enable(gpio_num, GPIO_OUTPUT);
            gpio_write(gpio_num, true);
        } else if (strcmp(pcParam[i], "off") == 0) {
            uint8_t gpio_num = atoi(pcValue[i]);
            gpio_enable(gpio_num, GPIO_OUTPUT);
            gpio_write(gpio_num, false);
        } else if (strcmp(pcParam[i], "toggle") == 0) {
            uint8_t gpio_num = atoi(pcValue[i]);
            gpio_enable(gpio_num, GPIO_OUTPUT);
            gpio_toggle(gpio_num);
        }
    }
    return "/index.ssi";
}

char *about_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[])
{
    return "/about.html";
}

char *websocket_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[])
{
    return "/websockets.html";
}

void websocket_task_measurements(void *pvParameter)
{
    printf("----------------------------websocket_task_measurements-------------------------\n");
    
    struct tcp_pcb *pcb = (struct tcp_pcb *) pvParameter;

    for (;;) {
        if (pcb == NULL || pcb->state != ESTABLISHED) {
            printf("-------------------------Connection closed, deleting task\n");
            break;
        }
	printf("----------------------------websocket_task_measurements-------------------------\n");
	for(int i = 0; i < 2 ; i++){
		if(!ConnectedDevices[i].initialized)
			break;
		char response[112] = "{";
        	int len = snprintf(response, sizeof (response),
                "{\"mac\" : \"%s\","
                " \"measurement\" : \"%s\"}", ConnectedDevices[i].device_mac, ConnectedDevices[i].last_measure);
        	if (len < sizeof (response))
            		websocket_write(pcb, (unsigned char *) response, len, WS_TEXT_MODE);
		printf("%s\n", response);
        	vTaskDelay(2000 / portTICK_PERIOD_MS);
	}
        /* Generate response in JSON format */
        

        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

void websocket_task(void *pvParameter)
{
    printf("websocket_task\n");
    struct tcp_pcb *pcb = (struct tcp_pcb *) pvParameter;

    for (;;) {
        if (pcb == NULL || pcb->state != ESTABLISHED) {
            printf("++++++++++++++++++++++++++Connection closed, deleting task\n");
            break;
        }

	printf("++++++++++++++++++++++++++++++websocket_task_measurements+++++++++++++++++++++++++++++++\n");
        int uptime = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        int heap = (int) xPortGetFreeHeapSize();
        int led = !gpio_read(LED_PIN);

        /* Generate response in JSON format */
        char response[64];
        int len = snprintf(response, sizeof (response),
                "{\"uptime\" : \"%d\","
                " \"heap\" : \"%d\","
                " \"led\" : \"%d\"}", uptime, heap, led);
        if (len < sizeof (response))
            websocket_write(pcb, (unsigned char *) response, len, WS_TEXT_MODE);

        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

/**
 * This function is called when websocket frame is received.
 *
 * Note: this function is executed on TCP thread and should return as soon
 * as possible.
 */
void websocket_cb(struct tcp_pcb *pcb, uint8_t *data, u16_t data_len, uint8_t mode)
{
    printf("[websocket_callback]:\n%.*s\n", (int) data_len, (char*) data);

    uint8_t response[18];
    response[0] = (uint8_t) 9;

    switch (data[0]) {
        case '0':
	    for(int i = 0; i < 2; i ++){
		response[i*9 + 0] = (uint8_t) i;
		
		uint8_t* byte = (uint8_t*) &ConnectedDevices[i].temperature;

		response[i*9 + 1] = byte[3];
		response[i*9 + 2] = byte[2];
		response[i*9 + 3] = byte[1];
		response[i*9 + 4] = byte[0];

		byte = (uint8_t*) &ConnectedDevices[i].air_pressure;

		response[i*9 + 5] = byte[3];
		response[i*9 + 6] = byte[2];
		response[i*9 + 7] = byte[1];
		response[i*9 + 8] = byte[0];
	    }
            break;
        case 'D': // Disable LED
            gpio_write(LED_PIN, true);
            //val = 0xDEAD;
            break;
        case 'E': // Enable LED
            gpio_write(LED_PIN, false);
            //val = 0xBEEF;
            break;
        default:
            printf("Unknown command\n");
            //val = 0;
            break;
    }
    websocket_write(pcb, response, 18, WS_BIN_MODE);
//
}

/**
 * This function is called when new websocket is open and
 * creates a new websocket_task if requested URI equals '/stream'.
 */
void websocket_open_cb(struct tcp_pcb *pcb, const char *uri)
{
    printf("WS URI: %s\n", uri);
    if (!strcmp(uri, "/stream")) {
        printf("request for streaming\n");
        //xTaskCreate(&websocket_task, "websocket_task", 256, (void *) pcb, 2, NULL);
        //xTaskCreate(&websocket_task_measurements, "websocket_task_measurements", 256, (void *) pcb, 3, NULL);
        //printf("request for streaming executed\n");
    }
}

void httpd_task(void *pvParameters)
{
    while(!MQTT_connection_established){
         printf("Delaying1\n");
         vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    printf("httpd_task continue...\n");

    tCGI pCGIs[] = {
        {"/gpio", (tCGIHandler) gpio_cgi_handler},
        {"/about", (tCGIHandler) about_cgi_handler},
        {"/websockets", (tCGIHandler) websocket_cgi_handler},
    };

    const char *pcConfigSSITags[] = {
        "uptime", // SSI_UPTIME
        "heap",   // SSI_FREE_HEAP
        "led"     // SSI_LED_STATE
    };

    /* register handlers and start the server */
    http_set_cgi_handlers(pCGIs, sizeof (pCGIs) / sizeof (pCGIs[0]));
    http_set_ssi_handler((tSSIHandler) ssi_handler, pcConfigSSITags, sizeof (pcConfigSSITags) / sizeof (pcConfigSSITags[0]));
    websocket_register_callbacks((tWsOpenHandler) websocket_open_cb, (tWsHandler) websocket_cb);
    httpd_init();

    printf("http handlers set \n");
    bmp280_init_successful = true;

    for (;;)
        vTaskDelay(200 / portTICK_PERIOD_MS);
}

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

static void topic_received(mqtt_message_data_t *md) {

    mqtt_message_t *message = md->message;
    int i;

    printf("Received: ");
    for (i = 0; i < md->topic->lenstring.len; ++i)
	printf("%c", md->topic->lenstring.data[i]);

    printf(" = ");
    for (i = 0; i < (int) message->payloadlen; ++i)
	printf("%c", ((char *) (message->payload))[i]);
    printf("\r\n");

    char current_device_mac[12];
    char temperature[6];
    char air_pressure[9];

    for(int j=0; j<12; j++)
	current_device_mac[j] = md->topic->lenstring.data[15+j];
    current_device_mac[11] = '\0';

    for(int j=0; j<6; j++)
	temperature[j] = ((char *) (message->payload))[18+j];
    temperature[5] = '\0';
    printf("temperature  %s \n", temperature);

    for(int j=0; j<9; j++)
	air_pressure[j] = ((char *) (message->payload))[43+j];
    air_pressure[8] = '\0';

    printf("air_pressure  %s \n", air_pressure);
	

    for(int l=0; l<2; l++){
	// if device not yet initialized
	if(!(ConnectedDevices[l]).initialized){

		printf("// if device not yet initialized \n");

		(ConnectedDevices[l]).initialized = true;
		for(int j=0; j<12; j++)
			(ConnectedDevices[l]).device_mac[j] = current_device_mac[j];

		for (int k = 0; k < (int) message->payloadlen; ++k)
			(ConnectedDevices[l]).last_measure[k] = ((char *)(message->payload))[k];

		ConnectedDevices[l].temperature = atof(temperature);
		ConnectedDevices[l].air_pressure = atof(air_pressure);

		printf("temperature: %f \n air_pressure: %f \n", ConnectedDevices[l].temperature, ConnectedDevices[l].air_pressure);
		
		for(int j=0; j<12; j++)
			printf("%c", (ConnectedDevices[l]).device_mac[j]);
		printf("\n");

		for (int k = 0; k < (int) message->payloadlen; ++k)
			printf("%c", (ConnectedDevices[l]).last_measure[k]);
		printf("\n");

		break;
	} 
	// if device ALREADY initialized
	else if (!strcmp((ConnectedDevices[l].device_mac + '\0'), (current_device_mac + '\0'))) {

		printf("// // if device ALREADY initialized \n");
		
		for (int k = 0; k < (int) message->payloadlen; ++k)
			(ConnectedDevices[l]).last_measure[k] = ((char *)(message->payload))[k];

		ConnectedDevices[l].temperature = atof(temperature);
		ConnectedDevices[l].air_pressure = atof(air_pressure);

		printf("temperature: %s  air_pressure: %s", temperature, air_pressure);

		for (int k = 0; k < (int) message->payloadlen; ++k)
			printf("%c", (ConnectedDevices[l]).last_measure[k]);
		printf("\n");

	
		break;

	}
	else {
		printf("// if device NOT ALREADY initialized \n");

		//for (int k = 0; k < 13; ++k)
			printf("%s", current_device_mac);
		printf("\n");

		
		//for (int k = 0; k < 13; ++k)
			printf("%s", ConnectedDevices[l].device_mac);
		printf("\n");
	}
    }
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

	//taskENTER_CRITICAL();
        printf("%s: connecting to MQTT server %s ... ", __func__, mqtt_server);
        ret = ssl_connect(ssl_connection, mqtt_server, MQTT_PORT);
	//taskEXIT_CRITICAL();

        if (ret) {
            printf("error: %d\n\r", ret);
            ssl_destroy(ssl_connection);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        printf("done connecting to MQTT server\n\r");
        mqtt_client_new(&client, &network, 5000, mqtt_buf, 100, mqtt_readbuf, 100);

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
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        printf("Done \n");
  
	mqtt_subscribe(&client, mqtt_subscribe_topic, MQTT_QOS1, topic_received);

	MQTT_connection_established = true;
        while (wifi_ok && !ssl_invalid) {
            printf("MQTT entering waiting for queue \n");
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
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            if (ret == MQTT_DISCONNECTED)
                break;
        }
        printf("Connection dropped, request restart\n\r");
        ssl_destroy(ssl_connection);
    }
}


static void bmp280_task_forced(void *pvParameters)
{	
    while(!MQTT_connection_established || !bmp280_init_successful){
            printf("Delaying2\n");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    printf("bmp280_task_forced continue... \n");
	
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
        
	//bmp280_init_successful = true;

        // Measure loop
        while(1) {
        	printf("Messuring... \n");
        	

            // Force measure
            if (!bmp280_force_measurement(&bmp280_dev)) {
                printf("Force measure init failed \n");
	    	vTaskDelay(MESURE_INTERVAL_MS / portTICK_PERIOD_MS);
                break;
            }
            
            // Wait for measurement
            while (bmp280_is_measuring(&bmp280_dev)) { vTaskDelay(500 / portTICK_PERIOD_MS); };

            if (!bmp280_read_float(&bmp280_dev, &temperature, &pressure, NULL)) {
                printf("Reading temp failed \n");
	    	vTaskDelay(MESURE_INTERVAL_MS / portTICK_PERIOD_MS);
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

    printf("Establishing WI-FI connection ... ");

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
    snprintf(mqtt_publish_topic, sizeof(mqtt_publish_topic), "sensor/outside/%s/measures", device_mac);
    snprintf(mqtt_subscribe_topic, sizeof(mqtt_subscribe_topic), "sensor/outside/+/measures");
                
    printf("Device MAC: ");
    printf(device_mac);
    printf("\n");
    printf("Publish topic: ");
    printf(mqtt_publish_topic);
    printf("\n");
    printf("Subscribe topic: ");
    printf(mqtt_subscribe_topic);
    printf("\n");
    
    
    // INIT I2C for sensors
    i2c_init(i2c_bus, scl_pin, sda_pin, I2C_FREQ_400K);
    
    // Create queue to hold sensor data
    xQueue = xQueueCreate(3, sizeof(struct Measure));

    /* turn off LED */
    gpio_enable(LED_PIN, GPIO_OUTPUT);
    gpio_write(LED_PIN, true);
    
    /* initialize tasks */
    xTaskCreate(&bmp280_task_forced, "bmp280_task_forced", 1048, NULL, 2, NULL);
    xTaskCreate(&mqtt_task, "mqtt_task", 2048, NULL, 2, NULL);
    xTaskCreate(&httpd_task, "httpd_task", 128, NULL, 3, NULL);

}
