#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#define CS_PIN CONFIG_CS_PIN    // fixed by iomux to achieve high speed transmission
#define RES_PIN CONFIG_RES_PIN
#define CD_PIN CONFIG_CD_PIN

#define SPI_FREQ CONFIG_SPI_FREQ
//#define SPI_CFG (SPISettings(SPI_FREQ, MSBFIRST, SPI_MODE3))

// one bit framebuffer
#define FB_WIDTH CONFIG_FB_WIDTH
#define FB_HEIGHT CONFIG_FB_HEIGHT
#define FB_SIZE FB_WIDTH*FB_HEIGHT


// Constants
#define LOW 0
#define HIGH 1

typedef unsigned char byte;

int b_x;
int b_y;


byte fillR = 0xFF;
byte fillG = 0xFF;
byte fillB = 0xFF;

const uint32_t tiles[] = {0x00000000, 0x747F1880, 0xE4BD1F00, 0x7C210780, 0xF4631F00, 0xFC3F0F80, 0xFC3F0800, 0x7C2F1780, 0x8C7F1880, 0x71084700, 0x71094400, 0x8DB14980, 0x84210F80, 0xDD6B1880, 0x8E6B3880, 0x74631700, 0xF47D0800, 0x64A56780, 0xF47D8B80, 0x7C1C1F00, 0xF9084200, 0x8C631700, 0x8C54A200, 0x8C6B5D80, 0xDA88AD80, 0x8A884200, 0xF8888F80, 0x21080200, 0x744C0200, 0x57D5F500, 0x00004400, 0x00000400, 0x01004000, 0x746B1700, 0x23084700, 0x744C8F80, 0x744D1700, 0x32A5F100, 0xF43C1700, 0x741D1700, 0xFC444200, 0x745D1700, 0x745C1700};

spi_device_handle_t vspi;
spi_bus_config_t bus_cfg;
spi_device_interface_config_t if_cfg;

bool framebuf[FB_SIZE];
byte color = 0xFF;

void set_gpio_dir(gpio_num_t ionum, gpio_mode_t mode) {
    esp_err_t err = gpio_set_direction(ionum, mode);
    if(err != ESP_OK) {
        printf("set_gpio_dir failed with err = %d.\n", err);
    }
}

void write_gpio(gpio_num_t ionum, uint32_t level) {
    esp_err_t err = gpio_set_level(ionum, level);
    if(err != ESP_OK) {
        printf("write_gpio failed with err = %d.\n", err);
    }
}

void delay_ms(uint32_t ms) {
    vTaskDelay(ms / portTICK_PERIOD_MS);
}

void sendCommand(byte com) {
    spi_transaction_t t;
    byte buf[1];
    buf[0] = com;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = buf;
    write_gpio(CD_PIN, LOW);
    spi_device_transmit(vspi, &t);
    //vspi->transfer(com);
}
void sendData(byte dat) {
    spi_transaction_t t;
    byte buf[1];
    buf[0] = dat;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = buf;
    write_gpio(CD_PIN, HIGH);
    spi_device_transmit(vspi, &t);
    //vspi->transfer(dat);
}

void beginSend() {
    write_gpio(CS_PIN, LOW);
}
void endSend() {
    write_gpio(CS_PIN, HIGH);
    //delayMicroseconds(10);
}

void displayInit() {
    beginSend();

    // Remap & Color depth setting
    sendCommand(0xA0);
    //sendData(0xB0);
    sendData(0x30);

    // Dim mode
    sendCommand(0xAB);
    sendData(0x00);    // reserved(0x00)
    sendData(0x7F);    // R
    sendData(0x7F);    // G
    sendData(0x7F);    // B
    sendData(0x10);    // precharge
    
    // Enable linear grayscale table
    sendCommand(0xB9);

    // Display On
    sendCommand(0xAF);

    endSend();
}

void fill(byte R, byte G, byte B) {
    beginSend();
    sendCommand(0x26);
    sendData(0x01);
    endSend();
    fillR = R;
    fillG = G;
    fillB = B;
}
void nofill() {
    beginSend();
    sendCommand(0x26);
    sendData(0x00);
    endSend();
}

void drawRect(byte x1, byte y1, byte x2, byte y2, byte R, byte G, byte B) {
    beginSend();
    sendCommand(0x22);
    sendData(x1);  //x1
    sendData(y1 + 0x04);  //y1
    sendData(x2);  //x2
    sendData(y2 + 0x04);  //y2 83
    sendData(R);  //borderColor3
    sendData(G);  //2
    sendData(B);  //1
    sendData(fillR);  //fillColor3
    sendData(fillG);  //2
    sendData(fillB);  //1
    endSend();
}

void clearScreen() {
    fill(0, 0, 0);
    drawRect(0, 0, 159, 127, 0, 0, 0);
    delay_ms(10);
}

void commitFramebuffer(bool * fb) {
    // Set column addr.
    beginSend();
    sendCommand(0x15);
    sendData(0x00);   // start
    sendData(0x9F);   // end

    // Set row addr.
    sendCommand(0x75);
    sendData(0x04);   // start
    sendData(0x83);   // end

    // Write to GDDRAM
    sendCommand(0x5C);
    for(int i=0; i<FB_WIDTH*FB_HEIGHT; i++) {
        if(framebuf[i]) {
            sendData(color);
        } else {
            sendData(0x00);
        }
    }
  
    endSend();
}

void clear_framebuffer(bool * fb) {
    for(int i=0; i<FB_WIDTH*FB_HEIGHT; i++) {
        framebuf[i] = 0;
    }
}

void blit_tile(bool * fb, uint32_t x, uint32_t y, uint32_t t_id) {
    int ix, iy;
    uint32_t t = tiles[t_id];
    for(iy = 0; iy < 5; iy++) {
        if(y+iy >= FB_HEIGHT || y+iy < 0) {
            continue;
        }
        for(ix = 0; ix < 5; ix++) {
            if(x+ix >= FB_WIDTH || x+ix < 0) {
                continue;
            }
            if(t & (0b10000000000000000000000000000000 >> (ix + 5*iy))) {
                fb[x+ix + FB_WIDTH*(y+iy)] = 1;
            } else {
                fb[x+ix + FB_WIDTH*(y+iy)] = 0;
            }
        }
    }
}

uint32_t char2tid(char c) {
    if('a' <= c && c <= 'z') {
        return (uint32_t)(c - 'a' + 0x01);
    }
    if('A' <= c && c <= 'Z') {
        return (uint32_t)(c - 'A' + 0x01);
    }
    if('0' <= c && c <= '9') {
        return (uint32_t)(c - '0' + 0x21);
    }
    
    switch(c) {
        case '!':
            return 0x1B;
            break;
        case '?':
            return 0x1C;
            break;
        case '#':
            return 0x1D;
            break;
        case ',':
            return 0x1E;
            break;
        case '.':
            return 0x1F;
            break;
        case ':':
            return 0x20;
            break;
        case ' ':
            return 0x00;
            break;
        default:
            return 0x1C;
            break;
    }
}

void blit_string(bool * fb, uint32_t x, uint32_t y, const char * str) {
    uint32_t buf[33];
    int d_x = x;
    int i;
    for(i = 0; str[i] != '\0' && i < 32; i++) {
        buf[i] = char2tid(str[i]);
    }
    buf[i] = 0xFF;
    for(i = 0; buf[i] != 0xFF; i++) {
        blit_tile(framebuf, d_x, y, buf[i]);
        d_x += 6;
    }
}

int start() {
    set_gpio_dir(CS_PIN, GPIO_MODE_OUTPUT);
    set_gpio_dir(RES_PIN, GPIO_MODE_OUTPUT);
    set_gpio_dir(CD_PIN, GPIO_MODE_OUTPUT);

    // Init SPI bus
    write_gpio(CS_PIN, HIGH);
    
    bus_cfg.miso_io_num = 19;
    bus_cfg.mosi_io_num = 23;
    bus_cfg.sclk_io_num = 18;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 0;
    
    if_cfg.clock_speed_hz = SPI_FREQ;
    if_cfg.mode = 3;
    if_cfg.spics_io_num = 5;
    if_cfg.queue_size = 8;
    
    esp_err_t err = spi_bus_initialize(VSPI_HOST, &bus_cfg, 1);
    if(err != ESP_OK) {
        printf("spi_bus_initialize() failed with err = %d.\n", err);
        return -1;
    }
    err = spi_bus_add_device(VSPI_HOST, &if_cfg, &vspi);
    if(err != ESP_OK) {
        printf("spi_bus_add_device() failed with err = %d.\n", err);
        return -1;
    }
    
    // Force reset
    write_gpio(RES_PIN, HIGH);
    delay_ms(1);
    write_gpio(RES_PIN, LOW);
    delay_ms(1);
    write_gpio(RES_PIN, HIGH);
    delay_ms(1000);
    
    displayInit();
    
    clearScreen();
    
    return 0;
}

void loop() {
    clear_framebuffer(framebuf);
    /*
    uint32_t mes[] = {0x08, 0x05, 0x0C, 0x0C, 0x0F, 0x1E, 0x00, 0x17, 0x0F, 0x12, 0x0C, 0x04, 0x1B, 0xFF};
    uint32_t d_x = 20;
    int i;
    for(i = 0; mes[i] != 0xFF; i++) {
        blit_tile(framebuf, d_x, 20, mes[i]);
        d_x += 6;
    }
    */
    blit_string(framebuf, 20, 20, "Hello, world!");
    blit_string(framebuf, 20, 30, "JST 16:00");
    blit_string(framebuf, 20, 36, "August 31st");
    blit_string(framebuf, 20, 50, "Humidity: 60%");
    
    color = 0b11111100;
    commitFramebuffer(framebuf);
    delay_ms(100);
}

void firm_task(void *pvParameter) {
    if(start() != 0) {
        return;
    }
    while(1) {
        loop();
    }
}

void app_main() {
    xTaskCreate(&firm_task, "firm_task", 64000, NULL, 5, NULL);
}
