# environmental-monitoring-system-zephyr-rtos

### Project Description
This project is a multi-threaded environmental monitoring system built on Zephyr RTOS. It measures ambient temperature and humidity using a DHT11 sensor and displays the values in real-time on a MAX7219 LED matrix. The system can be toggled on and off using a physical button.

### How it works
The system operation is based on several key mechanisms. A dedicated thread is responsible for fetching data from the DHT11 sensor at regular intervals. A second independent thread receives this sensor data through message queues and handles the real-time update of the 8x32 LED matrix. The entire monitoring logic and display can be enabled or disabled by a hardware interrupt that monitors the state of a physical user button. The hardware peripherals and pin configurations are defined and abstracted using DeviceTree overlays and Kconfig symbols.

### Hardware & Pins
The project is specifically designed to run on the ESP32-S3 DevKitC board. The DHT11 sensor is connected to GPIO 6 for data input. The system toggle function is mapped to a user button on GPIO 5 with an internal pull-up resistor. For the LED matrix communication, the MAX7219 driver utilizes GPIO 12 for the CLK, GPIO 11 for DIN, and GPIO 10 for CS signal.

### Build Instructions
To build the project using the specific overlay file, run the following command in your workspace:

```bash
west build -p always -b esp32s3_devkitc/esp32s3/procpu -- -DTC_OVERLAY_FILE=boards/esp32s3_devkitc.overlay
```

### Future Development
This project is under active development. Future updates will include server integration to allow for sending collected sensor data to a remote database. Additionally, an external comparison feature will be implemented, where local data will be compared with external outdoor temperature values retrieved from an online API or a secondary outdoor sensor.
