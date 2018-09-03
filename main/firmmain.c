#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_wps.h"
#include "esp_event_loop.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "lwip/err.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/netdb.h"

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

struct ntp_packet {
    uint8_t ntp_header; // Leap indicator(2bit), version(3bit) and mode(3bit)
    uint8_t layer;
    int8_t polling_freq;
    int8_t precision;
    uint32_t route_delay;
    uint32_t route_distrib;
    char reference_id[32 / sizeof(char)];
    uint64_t reference_ts;
    uint64_t start_ts;
    uint64_t receive_ts;
    uint64_t response_ts;
    //uint32_t auth_id;
    //uint32_t digest[4];
};

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

// PBC mode
static esp_wps_config_t wps_config = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
// Connection status
#define NET_DISCONNECTED 0
#define NET_WAITING_WPS 1
#define NET_AUTHORIZED 2
#define NET_CONNECTED 3
uint32_t net_stat = NET_DISCONNECTED;
ip_addr_t my_ip;
ip_addr_t ntp_server_ip;

// UDP IF
struct udp_pcb * udp_if;

// DNS resolve status
#define DNS_INIT 0
#define DNS_RESOLVING 1
#define DNS_NXDOMAIN 2
#define DNS_OK 3
#define DNS_FAIL 4
uint32_t dns_stat = DNS_INIT;
ip_addr_t dns_server_addr;

// SNTP
bool is_time_synced = false;
int r_timestamp;


// Function prototypes
void dns_callback(const char *, const ip_addr_t *, void *);
void recv_handler(void *, struct udp_pcb *, struct pbuf *, const ip_addr_t *, u16_t);

// GPIO Impl.
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

// Task Impl.
void delay_ms(uint32_t ms) {
    vTaskDelay(ms / portTICK_PERIOD_MS);
}

// Graphics Impl.
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

// Network Impl.
static esp_err_t event_handler(void * ctx, system_event_t * event) {
    esp_err_t err;
    switch(event->event_id) {
        case SYSTEM_EVENT_STA_START:
            puts("NET: Entered STA_START");
            net_stat = NET_WAITING_WPS;
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            printf("NET: Acquired IP addr. lease: %s.\n", ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
            ip4_addr_t ipv4_addr = event->event_info.got_ip.ip_info.ip;
            ip_addr_copy_from_ip4(my_ip, ipv4_addr);
            net_stat = NET_CONNECTED;
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            puts("NET: Entered STA_DISCONNECTED. Reconnecting...");
            net_stat = NET_DISCONNECTED;
            err = esp_wifi_connect();
            if(err != ESP_OK) {
                printf("NET: esp_wifi_connect() failed with err = %d.\n", err);
            }
            break;
        case SYSTEM_EVENT_STA_WPS_ER_SUCCESS:
            puts("NET: Entered STA_WPS_ER_SUCCESS.");
            net_stat = NET_AUTHORIZED;
            err = esp_wifi_wps_disable();
            if(err != ESP_OK) {
                printf("NET: esp_wifi_wps_disable() failed with err = %d.\n", err);
            }
            err = esp_wifi_connect();
            if(err != ESP_OK) {
                printf("NET: esp_wifi_connect() failed with err = %d.\n", err);
            }
            break;
        case SYSTEM_EVENT_STA_WPS_ER_FAILED:
            puts("NET: Entered STA_WPS_ER_FAILED.");
            err = esp_wifi_wps_disable();
            if(err != ESP_OK) {
                printf("NET: esp_wifi_wps_disable() failed with err = %d.\n", err);
            }
            err = esp_wifi_wps_enable(&wps_config);
            if(err != ESP_OK) {
                printf("NET: esp_wifi_wps_enable() failed with err = %d.\n", err);
            }
            err = esp_wifi_wps_start(0);
            if(err != ESP_OK) {
                printf("NET: esp_wifi_wps_start() failed with err = %d.\n", err);
            }
            break;
        case SYSTEM_EVENT_STA_WPS_ER_TIMEOUT:
            puts("NET: Entered STA_WPS_ER_TIMEOUT.");
            err = esp_wifi_wps_disable();
            if(err != ESP_OK) {
                printf("NET: esp_wifi_wps_disable() failed with err = %d.\n", err);
            }
            err = esp_wifi_wps_enable(&wps_config);
            if(err != ESP_OK) {
                printf("NET: esp_wifi_wps_enable() failed with err = %d.\n", err);
            }
            err = esp_wifi_wps_start(0);
            if(err != ESP_OK) {
                printf("NET: esp_wifi_wps_start() failed with err = %d.\n", err);
            }
            break;
        case SYSTEM_EVENT_STA_WPS_ER_PIN:
            puts("NET: Entered STA_WPS_ER_PIN.");
            break;
        default:
            break;
    }
    
    return ESP_OK;
}

// Main process
int start() {
    esp_err_t err;
    // Initialize NVS
    err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        puts("WARNING: NVS partition is full or corrupted. Erasing Flash...");
        err = nvs_flash_erase();
        if(err != ESP_OK) {
            printf("nvs_flash_erase() failed with err = %d.\n", err);
            return -1;
        }
    }
    if(err != ESP_OK) {
        printf("nvs_flash_init() failed with err = %d.\n", err);
        return -1;
    }
    
    // Initialize display driver
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
    
    err = spi_bus_initialize(VSPI_HOST, &bus_cfg, 1);
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
    
    // Clear screen
    clearScreen();
    
    
    // Initialize network
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    tcpip_adapter_init();
    err = esp_event_loop_init(event_handler, NULL);
    if(err != ESP_OK) {
        printf("esp_event_loop_init() failed with err = %d.\n", err);
        return -1;
    }
    err = esp_wifi_init(&wifi_cfg);
    if(err != ESP_OK) {
        printf("esp_wifi_init() failed with err = %d.\n", err);
        return -1;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if(err != ESP_OK) {
        printf("esp_wifi_set_mode() failed with err = %d.\n", err);
        return -1;
    }
    err = esp_wifi_start();
    if(err != ESP_OK) {
        printf("esp_wifi_start() failed with err = %d.\n", err);
        return -1;
    }
    // // Start WPS
    err = esp_wifi_wps_enable(&wps_config);
    if(err != ESP_OK) {
        printf("esp_wifi_wps_enable() failed with err = %d.\n", err);
        return -1;
    }
    err = esp_wifi_wps_start(0);
    if(err != ESP_OK) {
        printf("esp_wifi_wps_start() failed with err = %d.\n", err);
        return -1;
    }
    // DNS
    /*
    ip4_addr_t ipv4_dns_ip = {
        .addr = 0x08080808,
    };
    ip_addr_copy_from_ip4(dns_server_addr, ipv4_dns_ip);
    dns_setserver(0, &dns_server_addr);
    */
    
    // NTPC
    err_t lwip_err;
    udp_if = udp_new();
    lwip_err = udp_bind(udp_if, &my_ip, 55575);
    if(lwip_err != ERR_OK) {
        printf("udp_bind() failed with lwip_err = %d.\n", lwip_err);
    }
    return 0;
}

void loop() {
    char mes[33];
    
    err_t lwip_err;
    esp_err_t err;
    // Resolve host name
    if(net_stat == NET_CONNECTED && dns_stat == DNS_INIT) {
        ip4_addr_t ntp_ip4;
        err = mdns_init();
        if(err != ESP_OK) {
            printf("mdns_init() failed with err = %d.\n", err);
            dns_stat = DNS_FAIL;
            mdns_free();
            return;
        }
        //err = mdns_query_a("ikbk.net", 10000, &ntp_ip4);
        struct hostent * h_ent = gethostbyname("ikbk.net");
        if(h_errno == HOST_NOT_FOUND) {
            dns_stat = DNS_NXDOMAIN;
            mdns_free();
        } else if(h_ent == NULL) {
            printf("gethostbyname() failed with h_errno = %d.\n", h_errno);
            dns_stat = DNS_FAIL;
            mdns_free();
            return;
        } else {
            dns_stat = DNS_OK;
            uint8_t * temp = (const uint8_t *)h_ent->h_addr_list[0];
            int i=0;
            for(i=0; i<4; i++) {
                ((uint8_t *)&(ntp_ip4.addr))[3-i] = temp[i];
            }
            ip_addr_copy_from_ip4(ntp_server_ip, ntp_ip4);
            mdns_free();
        }
    }
    
    // NTP request
    if(net_stat == NET_CONNECTED && dns_stat == DNS_OK && !is_time_synced) {
        lwip_err = udp_connect(udp_if, &ntp_server_ip, 123);
        if(lwip_err != ERR_OK) {
            printf("udp_connect failed with lwip_err = %d.\n", lwip_err);
            is_time_synced = true;
            return;
        }
        byte sntp_buf[47];
        memset(sntp_buf, 0, 47);
        struct ntp_packet sntp_req = {
            .ntp_header = 0b00011011, // 00 011 011
            .layer = 2,
            .polling_freq = 6,
            .precision = -6,
        };
        struct pbuf * sntp_packet = pbuf_alloc(PBUF_TRANSPORT, sizeof(sntp_buf), PBUF_RAM);
        sntp_buf[0] = sntp_req.ntp_header;
        sntp_buf[1] = sntp_req.layer;
        sntp_buf[2] = sntp_req.polling_freq;
        for(int i=0;i<44;i++) {
            sntp_buf[i+3] = 0x00;
        }
        memcpy(sntp_packet->payload, sntp_buf, sizeof(sntp_buf));
        udp_recv(udp_if, recv_handler, NULL);
        ip4_addr_t ntp_v4 = ntp_server_ip.u_addr.ip4;
        printf("Sending to: %d\n", ntp_v4.addr);
        lwip_err = udp_send(udp_if, sntp_packet);
        pbuf_free(sntp_packet);
        if(lwip_err != ERR_OK) {
            printf("udp_send failed with lwip_err = %d.\n", lwip_err);
            is_time_synced = true;
            return;
        }
        is_time_synced = true;
    }
    
    clear_framebuffer(framebuf);
    switch(net_stat) {
        case NET_DISCONNECTED:
            blit_string(framebuf, 1, 1, "No Connection");
            break;
        case NET_WAITING_WPS:
            blit_string(framebuf, 1, 1, "Searching...");
            break;
        case NET_AUTHORIZED:
            blit_string(framebuf, 1, 1, "Requesting IP...");
            break;
        case NET_CONNECTED:
            blit_string(framebuf, 1, 1, "Wi-Fi");
            break;
        default:
            break;
    }
    blit_string(framebuf, 20, 20, "Hello, world!");
    sprintf(mes, "%d", r_timestamp);
    //blit_string(framebuf, 20, 30, "JST 16:00");
    blit_string(framebuf, 20, 30, mes);
    blit_string(framebuf, 20, 36, "August 31st");
    blit_string(framebuf, 20, 50, "Humidity: 60%");
    if(dns_stat == DNS_RESOLVING) {
        blit_string(framebuf, 20, 56, "Resolving...");
    } else if(dns_stat == DNS_NXDOMAIN) {
        blit_string(framebuf, 20, 56, "NTP server not found");
    } else if(dns_stat == DNS_OK) {
        blit_string(framebuf, 20, 56, "Found NTP server");
    } else {
        blit_string(framebuf, 20, 56, "DNS Error");
    }
    
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

// Callbacks
/*
void dns_callback(const char * name, const ip_addr_t * ipaddr, void * callback_arg) {
    if(ipaddr == NULL) {
        dns_stat = DNS_NXDOMAIN;
    } else {
        ntp_server_ip = *ipaddr;
        dns_stat = DNS_OK;
    }
}
*/

void recv_handler(void * arg, struct udp_pcb * pcb, struct pbuf * p, const ip_addr_t * addr, u16_t port) {
    if(port == 123) {
        byte recv_buf[sizeof(struct ntp_packet)];
        memset(recv_buf, 0, sizeof(recv_buf));
        int i=0;
        do {
            int j=0;
            while(j < p->len && i<sizeof(recv_buf)) {
                recv_buf[i++] = ((byte *)(p->payload))[j++];
            }
        } while((p = p->next) != NULL);
        r_timestamp = (recv_buf[40] & 0x000000FF) | ((recv_buf[41] << 8) & 0x0000FF00) | ((recv_buf[42] << 16) & 0x00FF0000) | ((recv_buf[43] << 24) & 0xFF000000);
        puts("OK.");
    }
}

