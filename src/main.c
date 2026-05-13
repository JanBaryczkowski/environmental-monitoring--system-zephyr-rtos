#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/display.h>
#include <string.h>
#include "wifi.h"
#include <zephyr/net/socket.h>
#include <stdio.h>

#define WIFI_SSID "ssid_sieci_do_poprawy"
#define WIFI_PSK "haslo_do_sieci"
#define SERVER_HOST "192.168.1.100"   //DO POPRAWY ALBO NP moj pc local
#define SERVER_PORT "8080"
#define SERVER_PATH "/data"

#define DEBOUNCE_MS 200
#define DHT_THREAD_STACK_SIZE 4096
#define MATRIX_THREAD_STACK_SIZE 4096
#define CLOUD_THREAD_STACK_SIZE 4096
#define DECISION_THREAD_STACK_SIZE 1024

static const struct device *dht_dev = DEVICE_DT_GET(DT_ALIAS(dht_sensor));
static const struct device *matrix_dev = DEVICE_DT_GET(DT_ALIAS(led_matrix));
static const struct gpio_dt_spec button	= GPIO_DT_SPEC_GET(DT_ALIAS(btn),gpios);

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led_heat), gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led_cold), gpios);

static atomic_t sys_enable = ATOMIC_INIT(0);
static int64_t last_press_time = 0;

static uint8_t matrix_buffer[32];	     // 4 segments x 8raws = 32 digits
struct gpio_callback btn_data;

// define stack areas  for the threads
K_THREAD_STACK_DEFINE(dht_stack, DHT_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(matrix_stack, MATRIX_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(cloud_stack, CLOUD_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(decision_stack, DECISION_THREAD_STACK_SIZE);

// define shared struct of sensor data
struct dht_data{
	volatile int16_t temp;
	volatile int16_t humid;
};

// enum for making decision by led
enum temp_decision{
    DECISION_NONE = 0,
    DECISION_HEAT,
    DECISION_COLD,
};

// define message queue
K_MSGQ_DEFINE(dht_msgq,
	    sizeof(struct dht_data),
	    10,
	    4);			// change from 4 to 8 - test

K_MSGQ_DEFINE(dht_cloud_msgq,
              sizeof(struct dht_data),
              10,
              4);

K_MSGQ_DEFINE(decision_msgq,
              sizeof(enum temp_decision),
              10,
              4);

// declare thread data structs
static struct k_thread dht_thread;
static struct k_thread matrix_thread;
static struct k_thread cloud_thread;
static struct k_thread decision_thread;

// numbers in matrix
static const uint8_t font3x5[10][5] = {		// 10 numbers and 5 raws
	[0] = {0b111, 0b101, 0b101, 0b101, 0b111},
	[1] = {0b010, 0b010, 0b010, 0b010, 0b010},
	[2] = {0b111, 0b001, 0b111, 0b100, 0b111},
	[3] = {0b111, 0b001, 0b111, 0b001, 0b111},
	[4] = {0b101, 0b101, 0b111, 0b001, 0b001},
	[5] = {0b111, 0b100, 0b111, 0b001, 0b111},
	[6] = {0b111, 0b100, 0b111, 0b101, 0b111},
	[7] = {0b111, 0b001, 0b001, 0b001, 0b001},
	[8] = {0b111, 0b101, 0b111, 0b101, 0b111},
	[9] = {0b111, 0b101, 0b111, 0b001, 0b111}
};

static const uint8_t font_degree = 0b11;
static const uint8_t font_celsius[5] = {0b111, 0b100, 0b100, 0b100, 0b111};
#define font_dot 0b00000001
static const uint8_t font_percent[5] = {0b100, 0b001, 0b010, 0b100, 0b001};


// callback function for interrupt
void btn_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins){

	int64_t now = k_uptime_get();

	if (now - last_press_time < DEBOUNCE_MS){
		return;
	}
	last_press_time = now;

	atomic_xor(&sys_enable, 1);
	if (atomic_get(&sys_enable)) {
		display_blanking_off(matrix_dev);
		printk("It is on babe\n");
	} else {
        	k_msgq_purge(&dht_msgq);

        	memset(matrix_buffer, 0, sizeof(matrix_buffer));

		const struct display_buffer_descriptor desc = {
			.buf_size = 32,
            		.width    = 8,
            		.height   = 32,
            		.pitch    = 8,
        	};
	display_write(matrix_dev, 0, 0, &desc, matrix_buffer);
	printk("Nah not again :(\n");
	}
}

// first thread
void dht_thread_start(void *arg_1, void *arg_2, void *arg_3){

	int ret;
	struct sensor_value temp_val, humid_val;
	struct dht_data data_to_send;

	while(1){
		if (!atomic_get(&sys_enable)){
			k_msleep(100);
			continue;
		}

		ret = sensor_sample_fetch(dht_dev);
		if (ret < 0) {
			printk("ERROR: sensor sample fetch failed");
			continue;
		} else {
			sensor_channel_get(dht_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp_val);
			sensor_channel_get(dht_dev, SENSOR_CHAN_HUMIDITY, &humid_val);

			data_to_send.temp  = (int16_t)(sensor_value_to_double(&temp_val)  * 10);
			data_to_send.humid = (int16_t)(sensor_value_to_double(&humid_val) * 10);

			if (k_msgq_put(&dht_msgq, &data_to_send, K_NO_WAIT) !=0) {
				printk("ERROR: sensor msgq full");
				continue;
			}

            if(k_msgq_put(&dht_cloud_msgq, &data_to_send, K_NO_WAIT) !=0){
                printk("ERROR: cloude msgq full");
                continue;
            }
		}
		k_msleep(200);		//debouncing
	}
}

// second thread
void matrix_thread_start(void *arg_1, void *arg_2, void *arg_3){
	struct dht_data data_recived;

	while(1){
// 		printk("matrix: waiting for data\n"); // just for debug
		if (k_msgq_get(&dht_msgq, &data_recived, K_FOREVER) == 0){
			if (!atomic_get(&sys_enable)) {
				k_msleep(100);
				continue;
			}
			printk("DHT11: Temp: %d.%d C | Hum: %d.%d %%\n",
				data_recived.temp  / 10, data_recived.temp  % 10,
				data_recived.humid / 10, data_recived.humid % 10);

               		memset(matrix_buffer, 0 , sizeof(matrix_buffer)); // buffer cleaning

			// tbh tem dec and humid dec is useless if you use dht11 and ofc i use it :)
			int temp_int = data_recived.temp / 10;
			int temp_tens = temp_int / 10;
			int temp_units = temp_int % 10;
			int temp_dec = data_recived.temp % 10;

			int humid_int = data_recived.humid / 10;
			int humid_tens = humid_int / 10;
			int humid_units = humid_int % 10;
			int humid_dec = data_recived.humid % 10;

			for (int i = 0; i < 5; i++){
				matrix_buffer[24 + i] = (font3x5[temp_tens][i] << 5) | (font3x5[temp_units][i] << 1);
			}

			matrix_buffer[28] |= font_dot;

			for (int i = 0; i < 5; i++) {
				matrix_buffer[16 + i] = (font3x5[temp_dec][i] << 5) | ((i< 2) ? (font_degree << 3) : 0) | (font_celsius[i]);
			}

			for (int i = 0; i < 5; i++) {
				matrix_buffer[8 + i] = (font3x5[humid_tens][i] << 5) | (font3x5[humid_units][i] << 1);
			}

			matrix_buffer[12] |= font_dot;

			for (int i = 0; i < 5 ; i++) {
				matrix_buffer[i] = (font3x5[humid_dec][i] << 5)  | (font_percent[i]);
			}

		       const struct display_buffer_descriptor desc = {
			       .buf_size = 32,
        			.width    = 8,
        			.height   = 32,
      				.pitch    = 8,
        		};

			display_write(matrix_dev, 0, 0, &desc, matrix_buffer);
		}
	}
}

// 3 THREAD to send data to server
void cloud_thread_start(void *arg_1, void *arg_2, void *arg_3){
	struct dht_data data_recived;
    char payload[128];
    char request[512];
    char response[256];
    enum temp_decision dec;

    struct zsock_addrinfo hints;        //hints for DNS lookup
    struct zsock_addrinfo *res;         //adres serwera - res

    int sock;
    int len;
    int ret;

	while(1) {
		if (k_msgq_get(&dht_cloud_msgq, &data_recived, K_FOREVER) != 0) {
            continue;
        }
        if (!atomic_get(&sys_enable)) {
				k_msleep(100);
				continue;
		}

        // building JSON
        snprintf(payload, sizeof(payload),
                 "{\"temp\": %d.%d, \"humid\": %d.%d}",
                 data_recived.temp / 10, data_recived.temp % 10,
                 data_recived.humid / 10, data_recived.humid % 10);

        // give name of server, by using hints and it automaticly search for ipv4 and tcp socket for DNS lookup and write it into struct hints
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;        // should be 0 sooo this is probably useless

        // DNS lookup
        ret = zsock_getaddrinfo(SERVER_HOST, SERVER_PORT, &hints, &res);
        if (ret != 0){
            printk("Error: DNS lookup failed (%d) \r\n", ret);
            k_msleep(5000);
            continue;
        }

        // create socket TCP, that is  already fullfilled by DNS lookup so no need to hardcoding it 
        sock = zsock_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock < 0){
            printk("Error: creating socket failed \r\n");
            zsock_freeaddrinfo(res);
            k_msleep(5000);
            continue;
        }

        // conect socket to server by using adress from dns lookup
        ret = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
        if (ret < 0) {
            printk("Error: connect failed \r\n");
            zsock_close(sock);
            zsock_freeaddrinfo(res);
            k_msleep(5000);
            continue;
        }

        zsock_freeaddrinfo(res);        //dns no longer needed

        // create json for http post request
        snprintf(request, sizeof(request),
                 "POST %s HTTP/1.0\r\n"
                 "Host: %s\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %d\r\n"
                 "\r\n"
                 "%s",
                 SERVER_PATH, SERVER_HOST,
                 (int)strlen(payload), payload);

        //send rquest to server
        ret = zsock_send(sock, request, strlen(request), 0);
        if (ret < 0) {
            printk("Error: send failed \r\n");
            zsock_close(sock);
            k_msleep(5000);
            continue;
        }

        char full_response[512];
        int total_len = 0;
        dec = DECISION_NONE;
        memset(full_response, 0, sizeof(full_response));

        while(1) {
            len = zsock_recv(sock, response, sizeof(response) - 1, 0);

            if (len < 0) {
                printk("Error: recive failed \r\n");
                break;
            }
            if (len == 0) {
                printk("Got response, end of connection");
                break;
            }
            if (total_len + len < sizeof(full_response) - 1) {
                memcpy(full_response + total_len, response, len);       // memcpy(direction, source, length in bytes)
                total_len += len;
            }
        }

        zsock_close(sock);

        if (strstr(full_response, "\"heat\"")) {
            dec = DECISION_HEAT;
        } else if (strstr(full_response, "\"cold\"")) {
            dec = DECISION_COLD;
        } else {
            dec = DECISION_NONE;
        }

        if (k_msgq_put(&decision_msgq, &dec, K_NO_WAIT) != 0){
            printk("Error: decision msgq is full \r\n");
        }

        k_msleep(5000);
	}
}

void decision_thread_start(void *arg_1, void *arg_2, void *arg_3) {
    enum temp_decision dec;

    while(1){
        if(k_msgq_get(&decision_msgq, &dec, K_FOREVER) !=0){
            continue;
        }

        if(!atomic_get(&sys_enable)){
            k_msleep(100);
            continue;
        }

        if(dec == DECISION_COLD){
            gpio_pin_set_dt(&led0, 1);
            gpio_pin_set_dt(&led1, 0);
            printk("Turn on cooling system");
        }
        if (dec == DECISION_HEAT){
            gpio_pin_set_dt(&led1, 1);
            gpio_pin_set_dt(&led0, 0);
            printk("Turn on heating system");
        }
        if (dec == DECISION_NONE){
            gpio_pin_set_dt(&led1, 0);
            gpio_pin_set_dt(&led0, 0);
            printk("Room temparature is okay");
        }

    }

}
int main(void){
	int ret;

	//ready ifs
       	if(!device_is_ready(dht_dev)){
               	printk("ERROR: dht sensor is not ready");
               	return 0;
       	};

       	if(!device_is_ready(matrix_dev)){
               	printk("ERROR: matrix is not ready");
               	return 0;
       	};

       	if(!gpio_is_ready_dt(&button)){
               	printk("ERROR: btn is not ready");
               	return 0;
       	};

	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret < 0) {
		return 0;
	}

	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        return 0;
    }

	if (!gpio_is_ready_dt(&led0)) {
		return 0;
	}

	ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return 0;
	}

	if (!gpio_is_ready_dt(&led1)) {
		return 0;
	}

	ret = gpio_pin_configure_dt(&led1, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return 0;
	}

	// interrupt
	gpio_init_callback(&btn_data, btn_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &btn_data);

    // wifi
    wifi_init();

    ret = wifi_connect(WIFI_SSID, WIFI_PSK);
    if (ret < 0){
        printk("ERROR: wifi_connect failed (%d)\r\n", ret);
        return 0;
    }

    wifi_wait_for_ip_addr();

	// start threads
    k_thread_create(&dht_thread,		//thread struct
  		      dht_stack,		       //stack
		      K_THREAD_STACK_SIZEOF(dht_stack),
		      dht_thread_start,	   //entry point
		      NULL, NULL, NULL,	   //args
	   	  5, 			           //priority
		      0, 			           // option
		      K_NO_WAIT);		       // start delay

    k_thread_create(&matrix_thread,
 		      matrix_stack,
		      K_THREAD_STACK_SIZEOF(matrix_stack),
	     	  matrix_thread_start,
		      NULL, NULL, NULL,
	  	      5,
		      0,
		      K_NO_WAIT);

    k_thread_create(&cloud_thread,
                    cloud_stack,
                    K_THREAD_STACK_SIZEOF(cloud_stack),
                    cloud_thread_start,
                    NULL, NULL, NULL,
                    5,
                    0,
                    K_NO_WAIT);

    k_thread_create(&decision_thread,
                    decision_stack,
                    K_THREAD_STACK_SIZEOF(decision_stack),
                    decision_thread_start,
                    NULL, NULL, NULL,
                    5,
                    0,
                    K_NO_WAIT);

	while(1){
		k_msleep(1000);
	}
	return 0;
}
