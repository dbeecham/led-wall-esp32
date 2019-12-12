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

    %%{
        machine nats;

        action debug {
            ESP_LOGI("nats_parse", "%c", *p);
        }

        action subscribe {
            ESP_LOGI("nats_task", "Subscribing to NATS topics...");
            bytes_written = write(sockfd, "SUB matrix1.in 1\r\n", strlen("SUB matrix1.in 1\r\n"));
            if (-1 == bytes_written || 0 == bytes_written) {
                ESP_LOGE("nats_task", "Failed to subscribe to matrix1.in!");
                esp_restart();
            }
        }

        action pong {
            ESP_LOGI("nats_task", "PONG");
            bytes_written = write(sockfd, "PONG\r\n", strlen("PONG\r\n"));
            if (-1 == bytes_written || 0 == bytes_written) {
                ESP_LOGE("nats_task", "Failed to PONG!");
                esp_restart();
            }
        }

        action copy_red {
            display_event.display_buf[color_i].r = *p;
        }

        action copy_green {
            display_event.display_buf[color_i].g = *p;
        }

        action copy_blue {
            display_event.display_buf[color_i].b = *p;
        }

        action display {
            xQueueSend(event_queue, &display_event, 0);
        }

        action zero_tv_sec {
            tv_sec_i = 0;
        }

        action copy_tv_sec {
            my_tv_sec.raw[tv_sec_i++] = *p;
        }

        action fin_tv_sec {
            display_event.tv.tv_sec = my_tv_sec.tv_sec;
        }

        action zero_tv_nsec {
            tv_nsec_i = 0;
        }

        action copy_tv_nsec {
            my_tv_nsec.raw[tv_nsec_i++] = *p;
        }

        action fin_tv_nsec {
            display_event.tv.tv_nsec = my_tv_nsec.tv_nsec;
        }

        msg := 
            ' matrix1.in 1 163\r\n' @{ color_i = 0; }
                any{8} >to(zero_tv_sec) $copy_tv_sec @fin_tv_sec
                any{8} >to(zero_tv_nsec) $copy_tv_nsec @fin_tv_nsec
                (
                  any $copy_red 
                  any $copy_green
                  any $copy_blue @{ color_i += 1; }
                ){49}
              '\r\n' @display @{ fgoto loop; }
              $err{ ESP_LOGE("nats_task_msg", "err: %c (0x%02x)", *p, *p); fgoto loop; };

        ping := '\r\n' @pong $err{ ESP_LOGE("nats_task_ping", "err: %c (0x%02x)", *p, *p); fgoto loop; } @{ fgoto loop; };

        info := ' {'
                (any - '}')*
                '}'
                ' '?
                '\r\n' $err{ ESP_LOGE("nats_task_info", "err: %c (0x%02x)", *p, *p); fgoto loop; }
                @{ fgoto loop; };

        loop :=
            ( 'INFO' @{ fgoto info; }
            | 'PING' @{ fgoto ping; }
            | 'MSG' @{ fgoto msg; }
            ) $err{ ESP_LOGE("nats_task", "err in loop: %c (0x%02x) in state %d", *p, *p, cs); fgoto loop; };

        main := 'INFO {'
                (any - '}')*
                '}'
                ' '?
                '\r\n' @subscribe $err{ ESP_LOGE("nats_task", "err: %c (0x%02x)", *p, *p); }
                '+OK\r\n' @{ fgoto loop; };

        write data;
        write init;

    }%%


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
            %% write exec;

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
