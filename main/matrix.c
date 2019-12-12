
#line 1 "main/matrix.c.rl"
// Some info:
// The SPI2 and SPI3 has some pins called IO_MUX pins, which allows for speeds
// up to 80MHz, compared to using a general GPIO, which "only" allows for 40
// MHz. For SPI2, the MOSI is pin 13, which is the one we use.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/rmt.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

// spi
#include <hal/spi_types.h>
#include <driver/spi_master.h>

spi_device_handle_t spi;

#define GPIO_PIN 2
#define GPIO_PIN_SEL (1ULL << GPIO_PIN)
#define RMT_CHANNEL 2
#define RMT_CLOCK_DIV 4
#define SPI_CHANNEL 1
#define SPI_CHANNEL_1_MOSI 12
#define SPI_CHANNEL_1_SCLK 14

// For these, one tick is 50ns.
#define RMT_TICKS_BIT_1_HIGH_WS2812 12 // 12*50ns = 600 ns
#define RMT_TICKS_BIT_1_LOW_WS2812  13 // 12*50ns = 650 ns
#define RMT_TICKS_BIT_0_HIGH_WS2812 5 // 12*50ns = 250 ns
#define RMT_TICKS_BIT_0_LOW_WS2812  20 // 20*50ns = 1000 ns
#define RMT_TICKS_RESET 510 // This isn't used now... i'm not sure what this value should be.

// This can be set pretty low, I don't remember what the exact number should
// be, check ws2811 datasheet.
#define LED_STRIP_REFRESH_PERIOD_MS (30U) 

#define WIFI_CONNECTED_BIT BIT0
#define NATS_CONNECTED_BIT BIT1
#define TIME_SYNC_BIT BIT2

#define NUM_PIXELS 49

#define NATS_HOST "192.168.4.1"
#define NATS_PORT "4222"
#define NATS_BUF_LEN 512

static EventGroupHandle_t s_wifi_event_group;
static QueueHandle_t event_queue;

uint32_t one  = SPI_SWAP_DATA_TX(0b11111111111111100000000000000000, 32);
uint32_t zero = SPI_SWAP_DATA_TX(0b11111100000000000000000000000000, 32);
//uint32_t one  = SPI_SWAP_DATA_TX(0b11101010101010101010101010101010, 32);
//uint32_t zero = SPI_SWAP_DATA_TX(0b11110000111100001111000011110000, 32);

struct matrix_rgb_s {
    uint8_t r;
    uint8_t g;
    uint8_t b;

};


struct display_event_s {
    struct timespec tv;
    struct matrix_rgb_s display_buf[NUM_PIXELS];
};

uint32_t rmt_items[24*NUM_PIXELS] = {0};


void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI("time_sync", "time is synced");
    xEventGroupSetBits(s_wifi_event_group, TIME_SYNC_BIT);
}


static void event_handler (
    void* arg,
    esp_event_base_t event_base, 
    int32_t event_id,
    void* event_data)
{

    // Connect to wifi
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI("H", "retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI("H", "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        // Initialize NTP
        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_setservername(0, "192.168.4.1");
        sntp_set_time_sync_notification_cb(time_sync_notification_cb);
        sntp_init();
    }
}


static void wifi_init_sta (
    void
)
{
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));


    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    // I don't care that this password is in plaintext; it's not a secure
    // wifi - it's only for the led wall.
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "led-wall",
            .password = "nallenalle"
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI("H", "wifi_init_sta finished.");
}

// This function takes an rgb display buffer and draws it on the display
// using an rmt channel.
static void matrix_display_draw_rgb (
    uint32_t * items,
    struct matrix_rgb_s * buf,
    uint32_t buf_len
)
{

    uint32_t rmt_i = 0;
    uint8_t bit;

    // For each pixel...
    for (int i = 0; i < buf_len; i++) {

        // Convert this pixels display buffer red to rmt_items.
        // For each bit in the buf[i].r, set the corresponding 
        for (bit = 8; bit > 0; bit--) {
            if ((buf[i].r >> (bit - 1)) & 1) {
                items[rmt_i] = one;
            } else {
                items[rmt_i] = zero;
            }
            rmt_i += 1;
        }

        // Same thing with green
        for (bit = 8; bit > 0; bit--) {
            if ((buf[i].g >> (bit - 1)) & 1) {
                items[rmt_i] = one;
            } else {
                items[rmt_i] = zero;
            }
            rmt_i += 1;
        }

        // And blue
        for (bit = 8; bit > 0; bit--) {
            if ((buf[i].b >> (bit - 1)) & 1) {
                items[rmt_i] = one;
            } else {
                items[rmt_i] = zero;
            }
            rmt_i += 1;
        }

        spi_device_queue_trans(spi, &(spi_transaction_t) {
            .tx_buffer = &rmt_items[rmt_i-24],
            .length = 32*24,
            .rxlength = 0
        }, 0);

    }

    // Finally, write out the buffer.
//    printf("%u:%u ", rmt_items[8], rmt_items[9]);
//    spi_device_transmit(spi, &(spi_transaction_t) {
//        .tx_buffer = rmt_items,
//        //.length = 32*24*NUM_PIXELS,
//        .length = 32*24,
//        .rxlength = 0
//    });
//    printf("(%u:%u)\n", rmt_items[8], rmt_items[9]);
}


static void nats_task (
    void * arg
)
{
    char buf[NATS_BUF_LEN];
    ssize_t bytes_read;
    ssize_t bytes_written;
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *servinfo, *ap;
    int sockfd = 0;
    char *p, *pe, *eof = NULL;
    int cs = 0;
    uint8_t color_i = 0;
    uint8_t tv_sec_i = 0;
    uint8_t tv_nsec_i = 0;

    union {
        long tv_sec;
        uint8_t raw[8];
    } my_tv_sec;

    union {
        long tv_nsec;
        uint8_t raw[8];
    } my_tv_nsec;

    struct display_event_s display_event = {0};

    
#line 258 "main/matrix.c"
static const int nats_start = 1;
static const int nats_first_final = 217;
static const int nats_error = 0;

static const int nats_en_msg = 16;
static const int nats_en_ping = 200;
static const int nats_en_info = 202;
static const int nats_en_loop = 208;
static const int nats_en_main = 1;


#line 270 "main/matrix.c"
	{
	cs = nats_start;
	}

#line 356 "main/matrix.c.rl"



    while (1) {

        // Wait until we're connected to the wifi
        xEventGroupWaitBits(
                /* event_group = */ s_wifi_event_group, 
                /* bit = */ WIFI_CONNECTED_BIT,
                false,
                true,
                portMAX_DELAY
        );
        ESP_LOGI("nats_task", "wifi is connected, connecting to nats at %s:%s...", NATS_HOST, NATS_PORT);
        xEventGroupWaitBits(
                /* event_group = */ s_wifi_event_group, 
                /* bit = */ TIME_SYNC_BIT,
                false,
                true,
                portMAX_DELAY
        );
        ESP_LOGI("nats_task", "ok we have time...");

        // Find address of the nats server
        int ret = getaddrinfo(NATS_HOST, NATS_PORT, &hints, &servinfo);
        if (0 != ret) {
            /* Failed to get address information. Print an error message,
             * sleep for an hour and then try again. */
            ESP_LOGI("esp_task", "getaddrinfo failed");
            //fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
            esp_restart();
        }

        // Loop over the results, try to connect to them.
        for (ap = servinfo; ap != NULL; ap = ap->ai_next) {
            sockfd = socket(ap->ai_family, ap->ai_socktype, ap->ai_protocol);
            if (-1 == sockfd) {
                ESP_LOGI("nats_task", "socket failed...");
                continue;
            }

            if (-1 == connect(sockfd, ap->ai_addr, ap->ai_addrlen)) {
                close(sockfd);
                ESP_LOGI("nats_task", "connect failed...");
                continue;
            }
        
            break;
        }
        freeaddrinfo(servinfo);

        if (NULL == ap) {
            ESP_LOGE("nats_task", "Connect failed!");
            esp_restart();
            // TODO: reset, or retry.
        }

        ESP_LOGI("nats_task", "Connected to NATS!");

        // Read loop on nats
        do {
            bytes_read = read(sockfd, buf, NATS_BUF_LEN);
            if (-1 == bytes_read) {
                ESP_LOGE("nats_task", "read returned -1");
                // todo: reset, or retry.
                esp_restart();
            }
            if (0 == bytes_read) {
                ESP_LOGE("nats_task", "connection to NATS closed!");
                // todo: reset, or retry.
                esp_restart();
            }

            p = buf;
            pe = buf + bytes_read;
            
#line 352 "main/matrix.c"
	{
	if ( p == pe )
		goto _test_eof;
	switch ( cs )
	{
case 1:
	if ( (*p) == 73 )
		goto st2;
	goto st0;
tr8:
#line 350 "main/matrix.c.rl"
	{ ESP_LOGE("nats_task", "err: %c (0x%02x)", *p, *p); }
	goto st0;
tr199:
#line 329 "main/matrix.c.rl"
	{ ESP_LOGE("nats_task_msg", "err: %c (0x%02x)", *p, *p); {goto st208;} }
	goto st0;
tr202:
#line 331 "main/matrix.c.rl"
	{ ESP_LOGE("nats_task_ping", "err: %c (0x%02x)", *p, *p); {goto st208;} }
	goto st0;
tr208:
#line 337 "main/matrix.c.rl"
	{ ESP_LOGE("nats_task_info", "err: %c (0x%02x)", *p, *p); {goto st208;} }
	goto st0;
tr212:
#line 344 "main/matrix.c.rl"
	{ ESP_LOGE("nats_task", "err in loop: %c (0x%02x) in state %d", *p, *p, cs); {goto st208;} }
	goto st0;
#line 382 "main/matrix.c"
st0:
cs = 0;
	goto _out;
st2:
	if ( ++p == pe )
		goto _test_eof2;
case 2:
	if ( (*p) == 78 )
		goto st3;
	goto st0;
st3:
	if ( ++p == pe )
		goto _test_eof3;
case 3:
	if ( (*p) == 70 )
		goto st4;
	goto st0;
st4:
	if ( ++p == pe )
		goto _test_eof4;
case 4:
	if ( (*p) == 79 )
		goto st5;
	goto st0;
st5:
	if ( ++p == pe )
		goto _test_eof5;
case 5:
	if ( (*p) == 32 )
		goto st6;
	goto st0;
st6:
	if ( ++p == pe )
		goto _test_eof6;
case 6:
	if ( (*p) == 123 )
		goto st7;
	goto st0;
st7:
	if ( ++p == pe )
		goto _test_eof7;
case 7:
	if ( (*p) == 125 )
		goto st8;
	goto st7;
st8:
	if ( ++p == pe )
		goto _test_eof8;
case 8:
	switch( (*p) ) {
		case 13: goto st9;
		case 32: goto st15;
	}
	goto tr8;
st9:
	if ( ++p == pe )
		goto _test_eof9;
case 9:
	if ( (*p) == 10 )
		goto tr11;
	goto tr8;
tr11:
#line 261 "main/matrix.c.rl"
	{
            ESP_LOGI("nats_task", "Subscribing to NATS topics...");
            bytes_written = write(sockfd, "SUB matrix1.in 1\r\n", strlen("SUB matrix1.in 1\r\n"));
            if (-1 == bytes_written || 0 == bytes_written) {
                ESP_LOGE("nats_task", "Failed to subscribe to matrix1.in!");
                esp_restart();
            }
        }
	goto st10;
st10:
	if ( ++p == pe )
		goto _test_eof10;
case 10:
#line 459 "main/matrix.c"
	if ( (*p) == 43 )
		goto st11;
	goto tr8;
st11:
	if ( ++p == pe )
		goto _test_eof11;
case 11:
	if ( (*p) == 79 )
		goto st12;
	goto st0;
st12:
	if ( ++p == pe )
		goto _test_eof12;
case 12:
	if ( (*p) == 75 )
		goto st13;
	goto st0;
st13:
	if ( ++p == pe )
		goto _test_eof13;
case 13:
	if ( (*p) == 13 )
		goto st14;
	goto st0;
st14:
	if ( ++p == pe )
		goto _test_eof14;
case 14:
	if ( (*p) == 10 )
		goto tr16;
	goto st0;
tr16:
#line 351 "main/matrix.c.rl"
	{ {goto st208;} }
	goto st217;
st217:
	if ( ++p == pe )
		goto _test_eof217;
case 217:
#line 499 "main/matrix.c"
	goto st0;
st15:
	if ( ++p == pe )
		goto _test_eof15;
case 15:
	if ( (*p) == 13 )
		goto st9;
	goto tr8;
st16:
	if ( ++p == pe )
		goto _test_eof16;
case 16:
	if ( (*p) == 32 )
		goto st17;
	goto st0;
st17:
	if ( ++p == pe )
		goto _test_eof17;
case 17:
	if ( (*p) == 109 )
		goto st18;
	goto st0;
st18:
	if ( ++p == pe )
		goto _test_eof18;
case 18:
	if ( (*p) == 97 )
		goto st19;
	goto st0;
st19:
	if ( ++p == pe )
		goto _test_eof19;
case 19:
	if ( (*p) == 116 )
		goto st20;
	goto st0;
st20:
	if ( ++p == pe )
		goto _test_eof20;
case 20:
	if ( (*p) == 114 )
		goto st21;
	goto st0;
st21:
	if ( ++p == pe )
		goto _test_eof21;
case 21:
	if ( (*p) == 105 )
		goto st22;
	goto st0;
st22:
	if ( ++p == pe )
		goto _test_eof22;
case 22:
	if ( (*p) == 120 )
		goto st23;
	goto st0;
st23:
	if ( ++p == pe )
		goto _test_eof23;
case 23:
	if ( (*p) == 49 )
		goto st24;
	goto st0;
st24:
	if ( ++p == pe )
		goto _test_eof24;
case 24:
	if ( (*p) == 46 )
		goto st25;
	goto st0;
st25:
	if ( ++p == pe )
		goto _test_eof25;
case 25:
	if ( (*p) == 105 )
		goto st26;
	goto st0;
st26:
	if ( ++p == pe )
		goto _test_eof26;
case 26:
	if ( (*p) == 110 )
		goto st27;
	goto st0;
st27:
	if ( ++p == pe )
		goto _test_eof27;
case 27:
	if ( (*p) == 32 )
		goto st28;
	goto st0;
st28:
	if ( ++p == pe )
		goto _test_eof28;
case 28:
	if ( (*p) == 49 )
		goto st29;
	goto st0;
st29:
	if ( ++p == pe )
		goto _test_eof29;
case 29:
	if ( (*p) == 32 )
		goto st30;
	goto st0;
st30:
	if ( ++p == pe )
		goto _test_eof30;
case 30:
	if ( (*p) == 49 )
		goto st31;
	goto st0;
st31:
	if ( ++p == pe )
		goto _test_eof31;
case 31:
	if ( (*p) == 54 )
		goto st32;
	goto st0;
st32:
	if ( ++p == pe )
		goto _test_eof32;
case 32:
	if ( (*p) == 51 )
		goto st33;
	goto st0;
st33:
	if ( ++p == pe )
		goto _test_eof33;
case 33:
	if ( (*p) == 13 )
		goto st34;
	goto st0;
st34:
	if ( ++p == pe )
		goto _test_eof34;
case 34:
	if ( (*p) == 10 )
		goto tr35;
	goto st0;
tr35:
#line 320 "main/matrix.c.rl"
	{ color_i = 0; }
	goto st35;
st35:
#line 295 "main/matrix.c.rl"
	{
            tv_sec_i = 0;
        }
	if ( ++p == pe )
		goto _test_eof35;
case 35:
#line 653 "main/matrix.c"
	goto tr36;
tr36:
#line 299 "main/matrix.c.rl"
	{
            my_tv_sec.raw[tv_sec_i++] = *p;
        }
	goto st36;
st36:
	if ( ++p == pe )
		goto _test_eof36;
case 36:
#line 665 "main/matrix.c"
	goto tr37;
tr37:
#line 299 "main/matrix.c.rl"
	{
            my_tv_sec.raw[tv_sec_i++] = *p;
        }
	goto st37;
st37:
	if ( ++p == pe )
		goto _test_eof37;
case 37:
#line 677 "main/matrix.c"
	goto tr38;
tr38:
#line 299 "main/matrix.c.rl"
	{
            my_tv_sec.raw[tv_sec_i++] = *p;
        }
	goto st38;
st38:
	if ( ++p == pe )
		goto _test_eof38;
case 38:
#line 689 "main/matrix.c"
	goto tr39;
tr39:
#line 299 "main/matrix.c.rl"
	{
            my_tv_sec.raw[tv_sec_i++] = *p;
        }
	goto st39;
st39:
	if ( ++p == pe )
		goto _test_eof39;
case 39:
#line 701 "main/matrix.c"
	goto tr40;
tr40:
#line 299 "main/matrix.c.rl"
	{
            my_tv_sec.raw[tv_sec_i++] = *p;
        }
	goto st40;
st40:
	if ( ++p == pe )
		goto _test_eof40;
case 40:
#line 713 "main/matrix.c"
	goto tr41;
tr41:
#line 299 "main/matrix.c.rl"
	{
            my_tv_sec.raw[tv_sec_i++] = *p;
        }
	goto st41;
st41:
	if ( ++p == pe )
		goto _test_eof41;
case 41:
#line 725 "main/matrix.c"
	goto tr42;
tr42:
#line 299 "main/matrix.c.rl"
	{
            my_tv_sec.raw[tv_sec_i++] = *p;
        }
	goto st42;
st42:
	if ( ++p == pe )
		goto _test_eof42;
case 42:
#line 737 "main/matrix.c"
	goto tr43;
tr43:
#line 299 "main/matrix.c.rl"
	{
            my_tv_sec.raw[tv_sec_i++] = *p;
        }
#line 303 "main/matrix.c.rl"
	{
            display_event.tv.tv_sec = my_tv_sec.tv_sec;
        }
	goto st43;
st43:
#line 307 "main/matrix.c.rl"
	{
            tv_nsec_i = 0;
        }
	if ( ++p == pe )
		goto _test_eof43;
case 43:
#line 757 "main/matrix.c"
	goto tr44;
tr44:
#line 311 "main/matrix.c.rl"
	{
            my_tv_nsec.raw[tv_nsec_i++] = *p;
        }
	goto st44;
st44:
	if ( ++p == pe )
		goto _test_eof44;
case 44:
#line 769 "main/matrix.c"
	goto tr45;
tr45:
#line 311 "main/matrix.c.rl"
	{
            my_tv_nsec.raw[tv_nsec_i++] = *p;
        }
	goto st45;
st45:
	if ( ++p == pe )
		goto _test_eof45;
case 45:
#line 781 "main/matrix.c"
	goto tr46;
tr46:
#line 311 "main/matrix.c.rl"
	{
            my_tv_nsec.raw[tv_nsec_i++] = *p;
        }
	goto st46;
st46:
	if ( ++p == pe )
		goto _test_eof46;
case 46:
#line 793 "main/matrix.c"
	goto tr47;
tr47:
#line 311 "main/matrix.c.rl"
	{
            my_tv_nsec.raw[tv_nsec_i++] = *p;
        }
	goto st47;
st47:
	if ( ++p == pe )
		goto _test_eof47;
case 47:
#line 805 "main/matrix.c"
	goto tr48;
tr48:
#line 311 "main/matrix.c.rl"
	{
            my_tv_nsec.raw[tv_nsec_i++] = *p;
        }
	goto st48;
st48:
	if ( ++p == pe )
		goto _test_eof48;
case 48:
#line 817 "main/matrix.c"
	goto tr49;
tr49:
#line 311 "main/matrix.c.rl"
	{
            my_tv_nsec.raw[tv_nsec_i++] = *p;
        }
	goto st49;
st49:
	if ( ++p == pe )
		goto _test_eof49;
case 49:
#line 829 "main/matrix.c"
	goto tr50;
tr50:
#line 311 "main/matrix.c.rl"
	{
            my_tv_nsec.raw[tv_nsec_i++] = *p;
        }
	goto st50;
st50:
	if ( ++p == pe )
		goto _test_eof50;
case 50:
#line 841 "main/matrix.c"
	goto tr51;
tr51:
#line 311 "main/matrix.c.rl"
	{
            my_tv_nsec.raw[tv_nsec_i++] = *p;
        }
#line 315 "main/matrix.c.rl"
	{
            display_event.tv.tv_nsec = my_tv_nsec.tv_nsec;
        }
	goto st51;
st51:
	if ( ++p == pe )
		goto _test_eof51;
case 51:
#line 857 "main/matrix.c"
	goto tr52;
tr52:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st52;
st52:
	if ( ++p == pe )
		goto _test_eof52;
case 52:
#line 869 "main/matrix.c"
	goto tr53;
tr53:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st53;
st53:
	if ( ++p == pe )
		goto _test_eof53;
case 53:
#line 881 "main/matrix.c"
	goto tr54;
tr54:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st54;
st54:
	if ( ++p == pe )
		goto _test_eof54;
case 54:
#line 895 "main/matrix.c"
	goto tr55;
tr55:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st55;
st55:
	if ( ++p == pe )
		goto _test_eof55;
case 55:
#line 907 "main/matrix.c"
	goto tr56;
tr56:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st56;
st56:
	if ( ++p == pe )
		goto _test_eof56;
case 56:
#line 919 "main/matrix.c"
	goto tr57;
tr57:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st57;
st57:
	if ( ++p == pe )
		goto _test_eof57;
case 57:
#line 933 "main/matrix.c"
	goto tr58;
tr58:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st58;
st58:
	if ( ++p == pe )
		goto _test_eof58;
case 58:
#line 945 "main/matrix.c"
	goto tr59;
tr59:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st59;
st59:
	if ( ++p == pe )
		goto _test_eof59;
case 59:
#line 957 "main/matrix.c"
	goto tr60;
tr60:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st60;
st60:
	if ( ++p == pe )
		goto _test_eof60;
case 60:
#line 971 "main/matrix.c"
	goto tr61;
tr61:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st61;
st61:
	if ( ++p == pe )
		goto _test_eof61;
case 61:
#line 983 "main/matrix.c"
	goto tr62;
tr62:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st62;
st62:
	if ( ++p == pe )
		goto _test_eof62;
case 62:
#line 995 "main/matrix.c"
	goto tr63;
tr63:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st63;
st63:
	if ( ++p == pe )
		goto _test_eof63;
case 63:
#line 1009 "main/matrix.c"
	goto tr64;
tr64:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st64;
st64:
	if ( ++p == pe )
		goto _test_eof64;
case 64:
#line 1021 "main/matrix.c"
	goto tr65;
tr65:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st65;
st65:
	if ( ++p == pe )
		goto _test_eof65;
case 65:
#line 1033 "main/matrix.c"
	goto tr66;
tr66:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st66;
st66:
	if ( ++p == pe )
		goto _test_eof66;
case 66:
#line 1047 "main/matrix.c"
	goto tr67;
tr67:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st67;
st67:
	if ( ++p == pe )
		goto _test_eof67;
case 67:
#line 1059 "main/matrix.c"
	goto tr68;
tr68:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st68;
st68:
	if ( ++p == pe )
		goto _test_eof68;
case 68:
#line 1071 "main/matrix.c"
	goto tr69;
tr69:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st69;
st69:
	if ( ++p == pe )
		goto _test_eof69;
case 69:
#line 1085 "main/matrix.c"
	goto tr70;
tr70:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st70;
st70:
	if ( ++p == pe )
		goto _test_eof70;
case 70:
#line 1097 "main/matrix.c"
	goto tr71;
tr71:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st71;
st71:
	if ( ++p == pe )
		goto _test_eof71;
case 71:
#line 1109 "main/matrix.c"
	goto tr72;
tr72:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st72;
st72:
	if ( ++p == pe )
		goto _test_eof72;
case 72:
#line 1123 "main/matrix.c"
	goto tr73;
tr73:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st73;
st73:
	if ( ++p == pe )
		goto _test_eof73;
case 73:
#line 1135 "main/matrix.c"
	goto tr74;
tr74:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st74;
st74:
	if ( ++p == pe )
		goto _test_eof74;
case 74:
#line 1147 "main/matrix.c"
	goto tr75;
tr75:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st75;
st75:
	if ( ++p == pe )
		goto _test_eof75;
case 75:
#line 1161 "main/matrix.c"
	goto tr76;
tr76:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st76;
st76:
	if ( ++p == pe )
		goto _test_eof76;
case 76:
#line 1173 "main/matrix.c"
	goto tr77;
tr77:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st77;
st77:
	if ( ++p == pe )
		goto _test_eof77;
case 77:
#line 1185 "main/matrix.c"
	goto tr78;
tr78:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st78;
st78:
	if ( ++p == pe )
		goto _test_eof78;
case 78:
#line 1199 "main/matrix.c"
	goto tr79;
tr79:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st79;
st79:
	if ( ++p == pe )
		goto _test_eof79;
case 79:
#line 1211 "main/matrix.c"
	goto tr80;
tr80:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st80;
st80:
	if ( ++p == pe )
		goto _test_eof80;
case 80:
#line 1223 "main/matrix.c"
	goto tr81;
tr81:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st81;
st81:
	if ( ++p == pe )
		goto _test_eof81;
case 81:
#line 1237 "main/matrix.c"
	goto tr82;
tr82:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st82;
st82:
	if ( ++p == pe )
		goto _test_eof82;
case 82:
#line 1249 "main/matrix.c"
	goto tr83;
tr83:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st83;
st83:
	if ( ++p == pe )
		goto _test_eof83;
case 83:
#line 1261 "main/matrix.c"
	goto tr84;
tr84:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st84;
st84:
	if ( ++p == pe )
		goto _test_eof84;
case 84:
#line 1275 "main/matrix.c"
	goto tr85;
tr85:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st85;
st85:
	if ( ++p == pe )
		goto _test_eof85;
case 85:
#line 1287 "main/matrix.c"
	goto tr86;
tr86:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st86;
st86:
	if ( ++p == pe )
		goto _test_eof86;
case 86:
#line 1299 "main/matrix.c"
	goto tr87;
tr87:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st87;
st87:
	if ( ++p == pe )
		goto _test_eof87;
case 87:
#line 1313 "main/matrix.c"
	goto tr88;
tr88:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st88;
st88:
	if ( ++p == pe )
		goto _test_eof88;
case 88:
#line 1325 "main/matrix.c"
	goto tr89;
tr89:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st89;
st89:
	if ( ++p == pe )
		goto _test_eof89;
case 89:
#line 1337 "main/matrix.c"
	goto tr90;
tr90:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st90;
st90:
	if ( ++p == pe )
		goto _test_eof90;
case 90:
#line 1351 "main/matrix.c"
	goto tr91;
tr91:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st91;
st91:
	if ( ++p == pe )
		goto _test_eof91;
case 91:
#line 1363 "main/matrix.c"
	goto tr92;
tr92:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st92;
st92:
	if ( ++p == pe )
		goto _test_eof92;
case 92:
#line 1375 "main/matrix.c"
	goto tr93;
tr93:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st93;
st93:
	if ( ++p == pe )
		goto _test_eof93;
case 93:
#line 1389 "main/matrix.c"
	goto tr94;
tr94:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st94;
st94:
	if ( ++p == pe )
		goto _test_eof94;
case 94:
#line 1401 "main/matrix.c"
	goto tr95;
tr95:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st95;
st95:
	if ( ++p == pe )
		goto _test_eof95;
case 95:
#line 1413 "main/matrix.c"
	goto tr96;
tr96:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st96;
st96:
	if ( ++p == pe )
		goto _test_eof96;
case 96:
#line 1427 "main/matrix.c"
	goto tr97;
tr97:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st97;
st97:
	if ( ++p == pe )
		goto _test_eof97;
case 97:
#line 1439 "main/matrix.c"
	goto tr98;
tr98:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st98;
st98:
	if ( ++p == pe )
		goto _test_eof98;
case 98:
#line 1451 "main/matrix.c"
	goto tr99;
tr99:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st99;
st99:
	if ( ++p == pe )
		goto _test_eof99;
case 99:
#line 1465 "main/matrix.c"
	goto tr100;
tr100:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st100;
st100:
	if ( ++p == pe )
		goto _test_eof100;
case 100:
#line 1477 "main/matrix.c"
	goto tr101;
tr101:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st101;
st101:
	if ( ++p == pe )
		goto _test_eof101;
case 101:
#line 1489 "main/matrix.c"
	goto tr102;
tr102:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st102;
st102:
	if ( ++p == pe )
		goto _test_eof102;
case 102:
#line 1503 "main/matrix.c"
	goto tr103;
tr103:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st103;
st103:
	if ( ++p == pe )
		goto _test_eof103;
case 103:
#line 1515 "main/matrix.c"
	goto tr104;
tr104:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st104;
st104:
	if ( ++p == pe )
		goto _test_eof104;
case 104:
#line 1527 "main/matrix.c"
	goto tr105;
tr105:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st105;
st105:
	if ( ++p == pe )
		goto _test_eof105;
case 105:
#line 1541 "main/matrix.c"
	goto tr106;
tr106:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st106;
st106:
	if ( ++p == pe )
		goto _test_eof106;
case 106:
#line 1553 "main/matrix.c"
	goto tr107;
tr107:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st107;
st107:
	if ( ++p == pe )
		goto _test_eof107;
case 107:
#line 1565 "main/matrix.c"
	goto tr108;
tr108:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st108;
st108:
	if ( ++p == pe )
		goto _test_eof108;
case 108:
#line 1579 "main/matrix.c"
	goto tr109;
tr109:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st109;
st109:
	if ( ++p == pe )
		goto _test_eof109;
case 109:
#line 1591 "main/matrix.c"
	goto tr110;
tr110:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st110;
st110:
	if ( ++p == pe )
		goto _test_eof110;
case 110:
#line 1603 "main/matrix.c"
	goto tr111;
tr111:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st111;
st111:
	if ( ++p == pe )
		goto _test_eof111;
case 111:
#line 1617 "main/matrix.c"
	goto tr112;
tr112:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st112;
st112:
	if ( ++p == pe )
		goto _test_eof112;
case 112:
#line 1629 "main/matrix.c"
	goto tr113;
tr113:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st113;
st113:
	if ( ++p == pe )
		goto _test_eof113;
case 113:
#line 1641 "main/matrix.c"
	goto tr114;
tr114:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st114;
st114:
	if ( ++p == pe )
		goto _test_eof114;
case 114:
#line 1655 "main/matrix.c"
	goto tr115;
tr115:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st115;
st115:
	if ( ++p == pe )
		goto _test_eof115;
case 115:
#line 1667 "main/matrix.c"
	goto tr116;
tr116:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st116;
st116:
	if ( ++p == pe )
		goto _test_eof116;
case 116:
#line 1679 "main/matrix.c"
	goto tr117;
tr117:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st117;
st117:
	if ( ++p == pe )
		goto _test_eof117;
case 117:
#line 1693 "main/matrix.c"
	goto tr118;
tr118:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st118;
st118:
	if ( ++p == pe )
		goto _test_eof118;
case 118:
#line 1705 "main/matrix.c"
	goto tr119;
tr119:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st119;
st119:
	if ( ++p == pe )
		goto _test_eof119;
case 119:
#line 1717 "main/matrix.c"
	goto tr120;
tr120:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st120;
st120:
	if ( ++p == pe )
		goto _test_eof120;
case 120:
#line 1731 "main/matrix.c"
	goto tr121;
tr121:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st121;
st121:
	if ( ++p == pe )
		goto _test_eof121;
case 121:
#line 1743 "main/matrix.c"
	goto tr122;
tr122:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st122;
st122:
	if ( ++p == pe )
		goto _test_eof122;
case 122:
#line 1755 "main/matrix.c"
	goto tr123;
tr123:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st123;
st123:
	if ( ++p == pe )
		goto _test_eof123;
case 123:
#line 1769 "main/matrix.c"
	goto tr124;
tr124:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st124;
st124:
	if ( ++p == pe )
		goto _test_eof124;
case 124:
#line 1781 "main/matrix.c"
	goto tr125;
tr125:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st125;
st125:
	if ( ++p == pe )
		goto _test_eof125;
case 125:
#line 1793 "main/matrix.c"
	goto tr126;
tr126:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st126;
st126:
	if ( ++p == pe )
		goto _test_eof126;
case 126:
#line 1807 "main/matrix.c"
	goto tr127;
tr127:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st127;
st127:
	if ( ++p == pe )
		goto _test_eof127;
case 127:
#line 1819 "main/matrix.c"
	goto tr128;
tr128:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st128;
st128:
	if ( ++p == pe )
		goto _test_eof128;
case 128:
#line 1831 "main/matrix.c"
	goto tr129;
tr129:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st129;
st129:
	if ( ++p == pe )
		goto _test_eof129;
case 129:
#line 1845 "main/matrix.c"
	goto tr130;
tr130:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st130;
st130:
	if ( ++p == pe )
		goto _test_eof130;
case 130:
#line 1857 "main/matrix.c"
	goto tr131;
tr131:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st131;
st131:
	if ( ++p == pe )
		goto _test_eof131;
case 131:
#line 1869 "main/matrix.c"
	goto tr132;
tr132:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st132;
st132:
	if ( ++p == pe )
		goto _test_eof132;
case 132:
#line 1883 "main/matrix.c"
	goto tr133;
tr133:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st133;
st133:
	if ( ++p == pe )
		goto _test_eof133;
case 133:
#line 1895 "main/matrix.c"
	goto tr134;
tr134:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st134;
st134:
	if ( ++p == pe )
		goto _test_eof134;
case 134:
#line 1907 "main/matrix.c"
	goto tr135;
tr135:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st135;
st135:
	if ( ++p == pe )
		goto _test_eof135;
case 135:
#line 1921 "main/matrix.c"
	goto tr136;
tr136:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st136;
st136:
	if ( ++p == pe )
		goto _test_eof136;
case 136:
#line 1933 "main/matrix.c"
	goto tr137;
tr137:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st137;
st137:
	if ( ++p == pe )
		goto _test_eof137;
case 137:
#line 1945 "main/matrix.c"
	goto tr138;
tr138:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st138;
st138:
	if ( ++p == pe )
		goto _test_eof138;
case 138:
#line 1959 "main/matrix.c"
	goto tr139;
tr139:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st139;
st139:
	if ( ++p == pe )
		goto _test_eof139;
case 139:
#line 1971 "main/matrix.c"
	goto tr140;
tr140:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st140;
st140:
	if ( ++p == pe )
		goto _test_eof140;
case 140:
#line 1983 "main/matrix.c"
	goto tr141;
tr141:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st141;
st141:
	if ( ++p == pe )
		goto _test_eof141;
case 141:
#line 1997 "main/matrix.c"
	goto tr142;
tr142:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st142;
st142:
	if ( ++p == pe )
		goto _test_eof142;
case 142:
#line 2009 "main/matrix.c"
	goto tr143;
tr143:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st143;
st143:
	if ( ++p == pe )
		goto _test_eof143;
case 143:
#line 2021 "main/matrix.c"
	goto tr144;
tr144:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st144;
st144:
	if ( ++p == pe )
		goto _test_eof144;
case 144:
#line 2035 "main/matrix.c"
	goto tr145;
tr145:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st145;
st145:
	if ( ++p == pe )
		goto _test_eof145;
case 145:
#line 2047 "main/matrix.c"
	goto tr146;
tr146:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st146;
st146:
	if ( ++p == pe )
		goto _test_eof146;
case 146:
#line 2059 "main/matrix.c"
	goto tr147;
tr147:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st147;
st147:
	if ( ++p == pe )
		goto _test_eof147;
case 147:
#line 2073 "main/matrix.c"
	goto tr148;
tr148:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st148;
st148:
	if ( ++p == pe )
		goto _test_eof148;
case 148:
#line 2085 "main/matrix.c"
	goto tr149;
tr149:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st149;
st149:
	if ( ++p == pe )
		goto _test_eof149;
case 149:
#line 2097 "main/matrix.c"
	goto tr150;
tr150:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st150;
st150:
	if ( ++p == pe )
		goto _test_eof150;
case 150:
#line 2111 "main/matrix.c"
	goto tr151;
tr151:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st151;
st151:
	if ( ++p == pe )
		goto _test_eof151;
case 151:
#line 2123 "main/matrix.c"
	goto tr152;
tr152:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st152;
st152:
	if ( ++p == pe )
		goto _test_eof152;
case 152:
#line 2135 "main/matrix.c"
	goto tr153;
tr153:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st153;
st153:
	if ( ++p == pe )
		goto _test_eof153;
case 153:
#line 2149 "main/matrix.c"
	goto tr154;
tr154:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st154;
st154:
	if ( ++p == pe )
		goto _test_eof154;
case 154:
#line 2161 "main/matrix.c"
	goto tr155;
tr155:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st155;
st155:
	if ( ++p == pe )
		goto _test_eof155;
case 155:
#line 2173 "main/matrix.c"
	goto tr156;
tr156:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st156;
st156:
	if ( ++p == pe )
		goto _test_eof156;
case 156:
#line 2187 "main/matrix.c"
	goto tr157;
tr157:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st157;
st157:
	if ( ++p == pe )
		goto _test_eof157;
case 157:
#line 2199 "main/matrix.c"
	goto tr158;
tr158:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st158;
st158:
	if ( ++p == pe )
		goto _test_eof158;
case 158:
#line 2211 "main/matrix.c"
	goto tr159;
tr159:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st159;
st159:
	if ( ++p == pe )
		goto _test_eof159;
case 159:
#line 2225 "main/matrix.c"
	goto tr160;
tr160:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st160;
st160:
	if ( ++p == pe )
		goto _test_eof160;
case 160:
#line 2237 "main/matrix.c"
	goto tr161;
tr161:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st161;
st161:
	if ( ++p == pe )
		goto _test_eof161;
case 161:
#line 2249 "main/matrix.c"
	goto tr162;
tr162:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st162;
st162:
	if ( ++p == pe )
		goto _test_eof162;
case 162:
#line 2263 "main/matrix.c"
	goto tr163;
tr163:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st163;
st163:
	if ( ++p == pe )
		goto _test_eof163;
case 163:
#line 2275 "main/matrix.c"
	goto tr164;
tr164:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st164;
st164:
	if ( ++p == pe )
		goto _test_eof164;
case 164:
#line 2287 "main/matrix.c"
	goto tr165;
tr165:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st165;
st165:
	if ( ++p == pe )
		goto _test_eof165;
case 165:
#line 2301 "main/matrix.c"
	goto tr166;
tr166:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st166;
st166:
	if ( ++p == pe )
		goto _test_eof166;
case 166:
#line 2313 "main/matrix.c"
	goto tr167;
tr167:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st167;
st167:
	if ( ++p == pe )
		goto _test_eof167;
case 167:
#line 2325 "main/matrix.c"
	goto tr168;
tr168:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st168;
st168:
	if ( ++p == pe )
		goto _test_eof168;
case 168:
#line 2339 "main/matrix.c"
	goto tr169;
tr169:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st169;
st169:
	if ( ++p == pe )
		goto _test_eof169;
case 169:
#line 2351 "main/matrix.c"
	goto tr170;
tr170:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st170;
st170:
	if ( ++p == pe )
		goto _test_eof170;
case 170:
#line 2363 "main/matrix.c"
	goto tr171;
tr171:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st171;
st171:
	if ( ++p == pe )
		goto _test_eof171;
case 171:
#line 2377 "main/matrix.c"
	goto tr172;
tr172:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st172;
st172:
	if ( ++p == pe )
		goto _test_eof172;
case 172:
#line 2389 "main/matrix.c"
	goto tr173;
tr173:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st173;
st173:
	if ( ++p == pe )
		goto _test_eof173;
case 173:
#line 2401 "main/matrix.c"
	goto tr174;
tr174:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st174;
st174:
	if ( ++p == pe )
		goto _test_eof174;
case 174:
#line 2415 "main/matrix.c"
	goto tr175;
tr175:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st175;
st175:
	if ( ++p == pe )
		goto _test_eof175;
case 175:
#line 2427 "main/matrix.c"
	goto tr176;
tr176:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st176;
st176:
	if ( ++p == pe )
		goto _test_eof176;
case 176:
#line 2439 "main/matrix.c"
	goto tr177;
tr177:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st177;
st177:
	if ( ++p == pe )
		goto _test_eof177;
case 177:
#line 2453 "main/matrix.c"
	goto tr178;
tr178:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st178;
st178:
	if ( ++p == pe )
		goto _test_eof178;
case 178:
#line 2465 "main/matrix.c"
	goto tr179;
tr179:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st179;
st179:
	if ( ++p == pe )
		goto _test_eof179;
case 179:
#line 2477 "main/matrix.c"
	goto tr180;
tr180:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st180;
st180:
	if ( ++p == pe )
		goto _test_eof180;
case 180:
#line 2491 "main/matrix.c"
	goto tr181;
tr181:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st181;
st181:
	if ( ++p == pe )
		goto _test_eof181;
case 181:
#line 2503 "main/matrix.c"
	goto tr182;
tr182:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st182;
st182:
	if ( ++p == pe )
		goto _test_eof182;
case 182:
#line 2515 "main/matrix.c"
	goto tr183;
tr183:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st183;
st183:
	if ( ++p == pe )
		goto _test_eof183;
case 183:
#line 2529 "main/matrix.c"
	goto tr184;
tr184:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st184;
st184:
	if ( ++p == pe )
		goto _test_eof184;
case 184:
#line 2541 "main/matrix.c"
	goto tr185;
tr185:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st185;
st185:
	if ( ++p == pe )
		goto _test_eof185;
case 185:
#line 2553 "main/matrix.c"
	goto tr186;
tr186:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st186;
st186:
	if ( ++p == pe )
		goto _test_eof186;
case 186:
#line 2567 "main/matrix.c"
	goto tr187;
tr187:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st187;
st187:
	if ( ++p == pe )
		goto _test_eof187;
case 187:
#line 2579 "main/matrix.c"
	goto tr188;
tr188:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st188;
st188:
	if ( ++p == pe )
		goto _test_eof188;
case 188:
#line 2591 "main/matrix.c"
	goto tr189;
tr189:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st189;
st189:
	if ( ++p == pe )
		goto _test_eof189;
case 189:
#line 2605 "main/matrix.c"
	goto tr190;
tr190:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st190;
st190:
	if ( ++p == pe )
		goto _test_eof190;
case 190:
#line 2617 "main/matrix.c"
	goto tr191;
tr191:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st191;
st191:
	if ( ++p == pe )
		goto _test_eof191;
case 191:
#line 2629 "main/matrix.c"
	goto tr192;
tr192:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st192;
st192:
	if ( ++p == pe )
		goto _test_eof192;
case 192:
#line 2643 "main/matrix.c"
	goto tr193;
tr193:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st193;
st193:
	if ( ++p == pe )
		goto _test_eof193;
case 193:
#line 2655 "main/matrix.c"
	goto tr194;
tr194:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st194;
st194:
	if ( ++p == pe )
		goto _test_eof194;
case 194:
#line 2667 "main/matrix.c"
	goto tr195;
tr195:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st195;
st195:
	if ( ++p == pe )
		goto _test_eof195;
case 195:
#line 2681 "main/matrix.c"
	goto tr196;
tr196:
#line 279 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].r = *p;
        }
	goto st196;
st196:
	if ( ++p == pe )
		goto _test_eof196;
case 196:
#line 2693 "main/matrix.c"
	goto tr197;
tr197:
#line 283 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].g = *p;
        }
	goto st197;
st197:
	if ( ++p == pe )
		goto _test_eof197;
case 197:
#line 2705 "main/matrix.c"
	goto tr198;
tr198:
#line 287 "main/matrix.c.rl"
	{
            display_event.display_buf[color_i].b = *p;
        }
#line 326 "main/matrix.c.rl"
	{ color_i += 1; }
	goto st198;
st198:
	if ( ++p == pe )
		goto _test_eof198;
case 198:
#line 2719 "main/matrix.c"
	if ( (*p) == 13 )
		goto st199;
	goto tr199;
st199:
	if ( ++p == pe )
		goto _test_eof199;
case 199:
	if ( (*p) == 10 )
		goto tr201;
	goto tr199;
tr201:
#line 291 "main/matrix.c.rl"
	{
            xQueueSend(event_queue, &display_event, 0);
        }
#line 328 "main/matrix.c.rl"
	{ {goto st208;} }
	goto st218;
st218:
	if ( ++p == pe )
		goto _test_eof218;
case 218:
#line 2742 "main/matrix.c"
	goto tr199;
st200:
	if ( ++p == pe )
		goto _test_eof200;
case 200:
	if ( (*p) == 13 )
		goto st201;
	goto tr202;
st201:
	if ( ++p == pe )
		goto _test_eof201;
case 201:
	if ( (*p) == 10 )
		goto tr204;
	goto tr202;
tr204:
#line 270 "main/matrix.c.rl"
	{
            ESP_LOGI("nats_task", "PONG");
            bytes_written = write(sockfd, "PONG\r\n", strlen("PONG\r\n"));
            if (-1 == bytes_written || 0 == bytes_written) {
                ESP_LOGE("nats_task", "Failed to PONG!");
                esp_restart();
            }
        }
#line 331 "main/matrix.c.rl"
	{ {goto st208;} }
	goto st219;
st219:
	if ( ++p == pe )
		goto _test_eof219;
case 219:
#line 2775 "main/matrix.c"
	goto tr202;
st202:
	if ( ++p == pe )
		goto _test_eof202;
case 202:
	if ( (*p) == 32 )
		goto st203;
	goto st0;
st203:
	if ( ++p == pe )
		goto _test_eof203;
case 203:
	if ( (*p) == 123 )
		goto st204;
	goto st0;
st204:
	if ( ++p == pe )
		goto _test_eof204;
case 204:
	if ( (*p) == 125 )
		goto st205;
	goto st204;
st205:
	if ( ++p == pe )
		goto _test_eof205;
case 205:
	switch( (*p) ) {
		case 13: goto st206;
		case 32: goto st207;
	}
	goto tr208;
st206:
	if ( ++p == pe )
		goto _test_eof206;
case 206:
	if ( (*p) == 10 )
		goto tr211;
	goto tr208;
tr211:
#line 338 "main/matrix.c.rl"
	{ {goto st208;} }
	goto st220;
st220:
	if ( ++p == pe )
		goto _test_eof220;
case 220:
#line 2822 "main/matrix.c"
	goto tr208;
st207:
	if ( ++p == pe )
		goto _test_eof207;
case 207:
	if ( (*p) == 13 )
		goto st206;
	goto tr208;
st208:
	if ( ++p == pe )
		goto _test_eof208;
case 208:
	switch( (*p) ) {
		case 73: goto st209;
		case 77: goto st212;
		case 80: goto st214;
	}
	goto tr212;
st209:
	if ( ++p == pe )
		goto _test_eof209;
case 209:
	if ( (*p) == 78 )
		goto st210;
	goto tr212;
st210:
	if ( ++p == pe )
		goto _test_eof210;
case 210:
	if ( (*p) == 70 )
		goto st211;
	goto tr212;
st211:
	if ( ++p == pe )
		goto _test_eof211;
case 211:
	if ( (*p) == 79 )
		goto tr218;
	goto tr212;
tr218:
#line 341 "main/matrix.c.rl"
	{ {goto st202;} }
	goto st221;
tr220:
#line 343 "main/matrix.c.rl"
	{ {goto st16;} }
	goto st221;
tr223:
#line 342 "main/matrix.c.rl"
	{ {goto st200;} }
	goto st221;
st221:
	if ( ++p == pe )
		goto _test_eof221;
case 221:
#line 2878 "main/matrix.c"
	goto tr212;
st212:
	if ( ++p == pe )
		goto _test_eof212;
case 212:
	if ( (*p) == 83 )
		goto st213;
	goto tr212;
st213:
	if ( ++p == pe )
		goto _test_eof213;
case 213:
	if ( (*p) == 71 )
		goto tr220;
	goto tr212;
st214:
	if ( ++p == pe )
		goto _test_eof214;
case 214:
	if ( (*p) == 73 )
		goto st215;
	goto tr212;
st215:
	if ( ++p == pe )
		goto _test_eof215;
case 215:
	if ( (*p) == 78 )
		goto st216;
	goto tr212;
st216:
	if ( ++p == pe )
		goto _test_eof216;
case 216:
	if ( (*p) == 71 )
		goto tr223;
	goto tr212;
	}
	_test_eof2: cs = 2; goto _test_eof; 
	_test_eof3: cs = 3; goto _test_eof; 
	_test_eof4: cs = 4; goto _test_eof; 
	_test_eof5: cs = 5; goto _test_eof; 
	_test_eof6: cs = 6; goto _test_eof; 
	_test_eof7: cs = 7; goto _test_eof; 
	_test_eof8: cs = 8; goto _test_eof; 
	_test_eof9: cs = 9; goto _test_eof; 
	_test_eof10: cs = 10; goto _test_eof; 
	_test_eof11: cs = 11; goto _test_eof; 
	_test_eof12: cs = 12; goto _test_eof; 
	_test_eof13: cs = 13; goto _test_eof; 
	_test_eof14: cs = 14; goto _test_eof; 
	_test_eof217: cs = 217; goto _test_eof; 
	_test_eof15: cs = 15; goto _test_eof; 
	_test_eof16: cs = 16; goto _test_eof; 
	_test_eof17: cs = 17; goto _test_eof; 
	_test_eof18: cs = 18; goto _test_eof; 
	_test_eof19: cs = 19; goto _test_eof; 
	_test_eof20: cs = 20; goto _test_eof; 
	_test_eof21: cs = 21; goto _test_eof; 
	_test_eof22: cs = 22; goto _test_eof; 
	_test_eof23: cs = 23; goto _test_eof; 
	_test_eof24: cs = 24; goto _test_eof; 
	_test_eof25: cs = 25; goto _test_eof; 
	_test_eof26: cs = 26; goto _test_eof; 
	_test_eof27: cs = 27; goto _test_eof; 
	_test_eof28: cs = 28; goto _test_eof; 
	_test_eof29: cs = 29; goto _test_eof; 
	_test_eof30: cs = 30; goto _test_eof; 
	_test_eof31: cs = 31; goto _test_eof; 
	_test_eof32: cs = 32; goto _test_eof; 
	_test_eof33: cs = 33; goto _test_eof; 
	_test_eof34: cs = 34; goto _test_eof; 
	_test_eof35: cs = 35; goto _test_eof; 
	_test_eof36: cs = 36; goto _test_eof; 
	_test_eof37: cs = 37; goto _test_eof; 
	_test_eof38: cs = 38; goto _test_eof; 
	_test_eof39: cs = 39; goto _test_eof; 
	_test_eof40: cs = 40; goto _test_eof; 
	_test_eof41: cs = 41; goto _test_eof; 
	_test_eof42: cs = 42; goto _test_eof; 
	_test_eof43: cs = 43; goto _test_eof; 
	_test_eof44: cs = 44; goto _test_eof; 
	_test_eof45: cs = 45; goto _test_eof; 
	_test_eof46: cs = 46; goto _test_eof; 
	_test_eof47: cs = 47; goto _test_eof; 
	_test_eof48: cs = 48; goto _test_eof; 
	_test_eof49: cs = 49; goto _test_eof; 
	_test_eof50: cs = 50; goto _test_eof; 
	_test_eof51: cs = 51; goto _test_eof; 
	_test_eof52: cs = 52; goto _test_eof; 
	_test_eof53: cs = 53; goto _test_eof; 
	_test_eof54: cs = 54; goto _test_eof; 
	_test_eof55: cs = 55; goto _test_eof; 
	_test_eof56: cs = 56; goto _test_eof; 
	_test_eof57: cs = 57; goto _test_eof; 
	_test_eof58: cs = 58; goto _test_eof; 
	_test_eof59: cs = 59; goto _test_eof; 
	_test_eof60: cs = 60; goto _test_eof; 
	_test_eof61: cs = 61; goto _test_eof; 
	_test_eof62: cs = 62; goto _test_eof; 
	_test_eof63: cs = 63; goto _test_eof; 
	_test_eof64: cs = 64; goto _test_eof; 
	_test_eof65: cs = 65; goto _test_eof; 
	_test_eof66: cs = 66; goto _test_eof; 
	_test_eof67: cs = 67; goto _test_eof; 
	_test_eof68: cs = 68; goto _test_eof; 
	_test_eof69: cs = 69; goto _test_eof; 
	_test_eof70: cs = 70; goto _test_eof; 
	_test_eof71: cs = 71; goto _test_eof; 
	_test_eof72: cs = 72; goto _test_eof; 
	_test_eof73: cs = 73; goto _test_eof; 
	_test_eof74: cs = 74; goto _test_eof; 
	_test_eof75: cs = 75; goto _test_eof; 
	_test_eof76: cs = 76; goto _test_eof; 
	_test_eof77: cs = 77; goto _test_eof; 
	_test_eof78: cs = 78; goto _test_eof; 
	_test_eof79: cs = 79; goto _test_eof; 
	_test_eof80: cs = 80; goto _test_eof; 
	_test_eof81: cs = 81; goto _test_eof; 
	_test_eof82: cs = 82; goto _test_eof; 
	_test_eof83: cs = 83; goto _test_eof; 
	_test_eof84: cs = 84; goto _test_eof; 
	_test_eof85: cs = 85; goto _test_eof; 
	_test_eof86: cs = 86; goto _test_eof; 
	_test_eof87: cs = 87; goto _test_eof; 
	_test_eof88: cs = 88; goto _test_eof; 
	_test_eof89: cs = 89; goto _test_eof; 
	_test_eof90: cs = 90; goto _test_eof; 
	_test_eof91: cs = 91; goto _test_eof; 
	_test_eof92: cs = 92; goto _test_eof; 
	_test_eof93: cs = 93; goto _test_eof; 
	_test_eof94: cs = 94; goto _test_eof; 
	_test_eof95: cs = 95; goto _test_eof; 
	_test_eof96: cs = 96; goto _test_eof; 
	_test_eof97: cs = 97; goto _test_eof; 
	_test_eof98: cs = 98; goto _test_eof; 
	_test_eof99: cs = 99; goto _test_eof; 
	_test_eof100: cs = 100; goto _test_eof; 
	_test_eof101: cs = 101; goto _test_eof; 
	_test_eof102: cs = 102; goto _test_eof; 
	_test_eof103: cs = 103; goto _test_eof; 
	_test_eof104: cs = 104; goto _test_eof; 
	_test_eof105: cs = 105; goto _test_eof; 
	_test_eof106: cs = 106; goto _test_eof; 
	_test_eof107: cs = 107; goto _test_eof; 
	_test_eof108: cs = 108; goto _test_eof; 
	_test_eof109: cs = 109; goto _test_eof; 
	_test_eof110: cs = 110; goto _test_eof; 
	_test_eof111: cs = 111; goto _test_eof; 
	_test_eof112: cs = 112; goto _test_eof; 
	_test_eof113: cs = 113; goto _test_eof; 
	_test_eof114: cs = 114; goto _test_eof; 
	_test_eof115: cs = 115; goto _test_eof; 
	_test_eof116: cs = 116; goto _test_eof; 
	_test_eof117: cs = 117; goto _test_eof; 
	_test_eof118: cs = 118; goto _test_eof; 
	_test_eof119: cs = 119; goto _test_eof; 
	_test_eof120: cs = 120; goto _test_eof; 
	_test_eof121: cs = 121; goto _test_eof; 
	_test_eof122: cs = 122; goto _test_eof; 
	_test_eof123: cs = 123; goto _test_eof; 
	_test_eof124: cs = 124; goto _test_eof; 
	_test_eof125: cs = 125; goto _test_eof; 
	_test_eof126: cs = 126; goto _test_eof; 
	_test_eof127: cs = 127; goto _test_eof; 
	_test_eof128: cs = 128; goto _test_eof; 
	_test_eof129: cs = 129; goto _test_eof; 
	_test_eof130: cs = 130; goto _test_eof; 
	_test_eof131: cs = 131; goto _test_eof; 
	_test_eof132: cs = 132; goto _test_eof; 
	_test_eof133: cs = 133; goto _test_eof; 
	_test_eof134: cs = 134; goto _test_eof; 
	_test_eof135: cs = 135; goto _test_eof; 
	_test_eof136: cs = 136; goto _test_eof; 
	_test_eof137: cs = 137; goto _test_eof; 
	_test_eof138: cs = 138; goto _test_eof; 
	_test_eof139: cs = 139; goto _test_eof; 
	_test_eof140: cs = 140; goto _test_eof; 
	_test_eof141: cs = 141; goto _test_eof; 
	_test_eof142: cs = 142; goto _test_eof; 
	_test_eof143: cs = 143; goto _test_eof; 
	_test_eof144: cs = 144; goto _test_eof; 
	_test_eof145: cs = 145; goto _test_eof; 
	_test_eof146: cs = 146; goto _test_eof; 
	_test_eof147: cs = 147; goto _test_eof; 
	_test_eof148: cs = 148; goto _test_eof; 
	_test_eof149: cs = 149; goto _test_eof; 
	_test_eof150: cs = 150; goto _test_eof; 
	_test_eof151: cs = 151; goto _test_eof; 
	_test_eof152: cs = 152; goto _test_eof; 
	_test_eof153: cs = 153; goto _test_eof; 
	_test_eof154: cs = 154; goto _test_eof; 
	_test_eof155: cs = 155; goto _test_eof; 
	_test_eof156: cs = 156; goto _test_eof; 
	_test_eof157: cs = 157; goto _test_eof; 
	_test_eof158: cs = 158; goto _test_eof; 
	_test_eof159: cs = 159; goto _test_eof; 
	_test_eof160: cs = 160; goto _test_eof; 
	_test_eof161: cs = 161; goto _test_eof; 
	_test_eof162: cs = 162; goto _test_eof; 
	_test_eof163: cs = 163; goto _test_eof; 
	_test_eof164: cs = 164; goto _test_eof; 
	_test_eof165: cs = 165; goto _test_eof; 
	_test_eof166: cs = 166; goto _test_eof; 
	_test_eof167: cs = 167; goto _test_eof; 
	_test_eof168: cs = 168; goto _test_eof; 
	_test_eof169: cs = 169; goto _test_eof; 
	_test_eof170: cs = 170; goto _test_eof; 
	_test_eof171: cs = 171; goto _test_eof; 
	_test_eof172: cs = 172; goto _test_eof; 
	_test_eof173: cs = 173; goto _test_eof; 
	_test_eof174: cs = 174; goto _test_eof; 
	_test_eof175: cs = 175; goto _test_eof; 
	_test_eof176: cs = 176; goto _test_eof; 
	_test_eof177: cs = 177; goto _test_eof; 
	_test_eof178: cs = 178; goto _test_eof; 
	_test_eof179: cs = 179; goto _test_eof; 
	_test_eof180: cs = 180; goto _test_eof; 
	_test_eof181: cs = 181; goto _test_eof; 
	_test_eof182: cs = 182; goto _test_eof; 
	_test_eof183: cs = 183; goto _test_eof; 
	_test_eof184: cs = 184; goto _test_eof; 
	_test_eof185: cs = 185; goto _test_eof; 
	_test_eof186: cs = 186; goto _test_eof; 
	_test_eof187: cs = 187; goto _test_eof; 
	_test_eof188: cs = 188; goto _test_eof; 
	_test_eof189: cs = 189; goto _test_eof; 
	_test_eof190: cs = 190; goto _test_eof; 
	_test_eof191: cs = 191; goto _test_eof; 
	_test_eof192: cs = 192; goto _test_eof; 
	_test_eof193: cs = 193; goto _test_eof; 
	_test_eof194: cs = 194; goto _test_eof; 
	_test_eof195: cs = 195; goto _test_eof; 
	_test_eof196: cs = 196; goto _test_eof; 
	_test_eof197: cs = 197; goto _test_eof; 
	_test_eof198: cs = 198; goto _test_eof; 
	_test_eof199: cs = 199; goto _test_eof; 
	_test_eof218: cs = 218; goto _test_eof; 
	_test_eof200: cs = 200; goto _test_eof; 
	_test_eof201: cs = 201; goto _test_eof; 
	_test_eof219: cs = 219; goto _test_eof; 
	_test_eof202: cs = 202; goto _test_eof; 
	_test_eof203: cs = 203; goto _test_eof; 
	_test_eof204: cs = 204; goto _test_eof; 
	_test_eof205: cs = 205; goto _test_eof; 
	_test_eof206: cs = 206; goto _test_eof; 
	_test_eof220: cs = 220; goto _test_eof; 
	_test_eof207: cs = 207; goto _test_eof; 
	_test_eof208: cs = 208; goto _test_eof; 
	_test_eof209: cs = 209; goto _test_eof; 
	_test_eof210: cs = 210; goto _test_eof; 
	_test_eof211: cs = 211; goto _test_eof; 
	_test_eof221: cs = 221; goto _test_eof; 
	_test_eof212: cs = 212; goto _test_eof; 
	_test_eof213: cs = 213; goto _test_eof; 
	_test_eof214: cs = 214; goto _test_eof; 
	_test_eof215: cs = 215; goto _test_eof; 
	_test_eof216: cs = 216; goto _test_eof; 

	_test_eof: {}
	if ( p == eof )
	{
	switch ( cs ) {
	case 198: 
	case 199: 
#line 329 "main/matrix.c.rl"
	{ ESP_LOGE("nats_task_msg", "err: %c (0x%02x)", *p, *p); {       if ( p == pe )
               goto _test_eof208;
goto st208;} }
	break;
	case 200: 
	case 201: 
#line 331 "main/matrix.c.rl"
	{ ESP_LOGE("nats_task_ping", "err: %c (0x%02x)", *p, *p); {       if ( p == pe )
               goto _test_eof208;
goto st208;} }
	break;
	case 205: 
	case 206: 
	case 207: 
#line 337 "main/matrix.c.rl"
	{ ESP_LOGE("nats_task_info", "err: %c (0x%02x)", *p, *p); {       if ( p == pe )
               goto _test_eof208;
goto st208;} }
	break;
	case 208: 
	case 209: 
	case 210: 
	case 211: 
	case 212: 
	case 213: 
	case 214: 
	case 215: 
	case 216: 
#line 344 "main/matrix.c.rl"
	{ ESP_LOGE("nats_task", "err in loop: %c (0x%02x) in state %d", *p, *p, cs); {       if ( p == pe )
               goto _test_eof208;
goto st208;} }
	break;
	case 8: 
	case 9: 
	case 10: 
	case 15: 
#line 350 "main/matrix.c.rl"
	{ ESP_LOGE("nats_task", "err: %c (0x%02x)", *p, *p); }
	break;
#line 3184 "main/matrix.c"
	}
	}

	_out: {}
	}

#line 432 "main/matrix.c.rl"

        } while(1);

    }

}


static void led_task (
    void * arg
)
{

    BaseType_t qres;
    struct display_event_s display_event = {0};
    struct timespec tv;
    int64_t tv_sec_diff;
    int64_t tv_nsec_diff;
    uint32_t sleep_ms;


    while(1) {
        qres = xQueueReceive(event_queue, &display_event, portMAX_DELAY);
        if (pdFALSE == qres) {
            continue;
        }

        // Wait until it's time to display this event...
        clock_gettime(CLOCK_REALTIME, &tv);
        tv_sec_diff = display_event.tv.tv_sec - tv.tv_sec;
        tv_nsec_diff = display_event.tv.tv_nsec - tv.tv_nsec;

        if (tv_sec_diff < 0 || (0 == tv_sec_diff && tv_nsec_diff < 0)) {
            // We already missed this event - just skip it.
            printf("missed event - supposed to be at %ld, but we're at %ld\n", display_event.tv.tv_sec, tv.tv_sec);
            continue;
        }

        sleep_ms = tv_sec_diff*1000 + tv_nsec_diff/1000000;
        if (sleep_ms > 3000) sleep_ms = 3000;

        vTaskDelay(sleep_ms / portTICK_PERIOD_MS);

        //vTaskSuspendAll();
        matrix_display_draw_rgb(rmt_items, display_event.display_buf, NUM_PIXELS);
        //xTaskResumeAll();
    }
}


void app_main (
    void
)
{


    // Initialize nvs
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


    // Initialize queue
    event_queue = xQueueCreate(128, sizeof(struct display_event_s));
    // TODO: error check


    ret = spi_bus_initialize(
        /* spi_host_device_t host = */ SPI2_HOST,
        /* spi_bus_config_t * config = */ &(spi_bus_config_t) {
            .miso_io_num = -1,
            .mosi_io_num = SPI_CHANNEL_1_MOSI,
            .sclk_io_num = SPI_CHANNEL_1_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 8192
        },
        /* dma_chan = */ 1
    );
    if (ESP_OK != ret) {
        ESP_LOGE(__func__, "Could not initialize SPI bus.");
        return -1;
    }

    ret = spi_bus_add_device(
        /* spi_host_device_t host = */ SPI2_HOST,
        /* spi_device_interface_config_t * config = */ &(spi_device_interface_config_t) {
            .command_bits = 0,
            .address_bits = 0,
            .dummy_bits = 0,
            .clock_speed_hz = 25641025,
            .mode = 1,
            .spics_io_num = -1,
            .queue_size = NUM_PIXELS,
            .cs_ena_posttrans = 0,
            .cs_ena_pretrans = 0,
            .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE,
            .input_delay_ns = 0
        },
        /* spi_device_handle_t * handle = */ &spi
    );
    if (ESP_OK != ret) {
        ESP_LOGE(__func__, "spi_bus_add_device() returned %d", ret);
        return -1;
    }

//    while (true) {
//        spi_device_transmit(spi, &(spi_transaction_t) {
//            .tx_buffer = &(uint32_t[4]){spi_ws2811_zero, spi_ws2811_one, spi_ws2811_zero, spi_ws2811_one},
//            .length = 32*2,
//            .rxlength = 0
//        });
//        vTaskDelay(50 / portTICK_PERIOD_MS);
//    }


    // Core 0 gets wifi interrupts
    xTaskCreatePinnedToCore(
        led_task,
        "ledtask",
        2096,
        NULL,
        1,
        NULL,
        1
    );

    xTaskCreatePinnedToCore(
        nats_task,
        "ledtask",
        4096,
        NULL,
        0,
        NULL,
        0
    );
   wifi_init_sta();

}
