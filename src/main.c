#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/display.h>
#include <string.h>

static const struct device *dht_dev = DEVICE_DT_GET(DT_ALIAS(dht_sensor));
static const struct device *matrix_dev = DEVICE_DT_GET(DT_ALIAS(led_matrix));
static const struct gpio_dt_spec button	= GPIO_DT_SPEC_GET(DT_ALIAS(btn),gpios);

static atomic_t sys_enable = ATOMIC_INIT(0);

#define DEBOUNCE_MS 200
static int64_t last_press_time = 0;

#define DHT_THREAD_STACK_SIZE 4096
#define MATRIX_THREAD_STACK_SIZE 4096

// define stack areas  for the threads
K_THREAD_STACK_DEFINE(dht_stack, DHT_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(matrix_stack, MATRIX_THREAD_STACK_SIZE);

// define shard struct of sensor data
struct dht_data{
	volatile int16_t temp;
	volatile int16_t humid;
};

// define message queue
K_MSGQ_DEFINE(dht_msgq,
	    sizeof(struct dht_data),
	    10,
	    4);			// change from 4 to 8 - test

// declare thread data structs
static struct k_thread dht_thread;
static struct k_thread matrix_thread;

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

static uint8_t matrix_buffer[32];	     // 4 segments x 8raws = 32 digits

struct gpio_callback btn_data;

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
            .width = 8,
            .height = 32,
            .pitch = 8,
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
		}
		k_msleep(200);		//debouncing
	}
}

// second thread

void matrix_thread_start(void *arg_1, void *arg_2, void *arg_3){
	struct dht_data data_recived;

	while(1){
// 		printk("matrix: waiting for data\n"); // just for debug
		if (k_msgq_get(&dht_msgq, &data_recived, K_MSEC(100)) == 0){
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
        			.width = 8,
        			.height = 32,
      				.pitch = 8,
        		};

			display_write(matrix_dev, 0, 0, &desc, matrix_buffer);
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
        };

	// interrupt
	gpio_init_callback(&btn_data, btn_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &btn_data);

	// start threads
	k_thread_create(&dht_thread,		//thread struct
  		      dht_stack,		//stack
		      K_THREAD_STACK_SIZEOF(dht_stack),
		      dht_thread_start,	//entry point
		      NULL, NULL, NULL,	//args
	   	      5, 			//priority
		      0, 			// option
		      K_NO_WAIT);		// start delay

	k_thread_create(&matrix_thread,
 		      matrix_stack,
		      K_THREAD_STACK_SIZEOF(matrix_stack),
	     	  matrix_thread_start,
		      NULL, NULL, NULL,
	  	      5,
		      0,
		      K_NO_WAIT);
	while(1){
		k_msleep(1000);
	}

	return 0;
}
