#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/rmt.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#define GPIO_PIN 2
#define GPIO_PIN_SEL (1ULL << GPIO_PIN)
#define RMT_CHANNEL 1
#define RMT_CLOCK_DIV 4

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

#define NUM_PIXELS 50

#define NATS_HOST "192.168.4.1"
#define NATS_PORT "4222"
#define NATS_BUF_LEN 512

static EventGroupHandle_t s_wifi_event_group;
static QueueHandle_t event_queue;

rmt_item32_t one = {
        .level0 = 1,
        .duration0 = RMT_TICKS_BIT_1_HIGH_WS2812,
        .level1 = 0,
        .duration1 = RMT_TICKS_BIT_1_LOW_WS2812
};

rmt_item32_t zero = {
        .level0 = 1,
        .duration0 = RMT_TICKS_BIT_0_HIGH_WS2812,
        .level1 = 0,
        .duration1 = RMT_TICKS_BIT_0_LOW_WS2812
};

rmt_item32_t reset = {
        .level0 = 0,
        .duration0 = 0,
        .level1 = 0,
        .duration1 = RMT_TICKS_RESET
};


struct matrix_rgb_s {
    uint8_t r;
    uint8_t g;
    uint8_t b;

};

struct matrix_rgb_s display_buf[NUM_PIXELS] = {0};
rmt_item32_t rmt_items[24*NUM_PIXELS] = {0};


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
    rmt_item32_t * items,
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
    }

    // Finally, write out the buffer.
    rmt_write_items(
        /* rmt_channel_t channel = */ RMT_CHANNEL,
        /* rmt_item32_t * rmt_items = */ rmt_items,
        /* item_len = */ 24*buf_len,
        /* bool wait_tx_done = */ true
    );

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
            display_buf[color_i].r = *p;
        }

        action copy_green {
            display_buf[color_i].g = *p;
        }

        action copy_blue {
            display_buf[color_i].b = *p;
        }

        action display {
            xQueueSend(event_queue, &(int){1}, 1);
        }

        msg := 
            ' matrix1.in 1 147\r\n' @{ color_i = 0; }
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
    uint_fast8_t event;

    while(1) {
        qres = xQueueReceive(event_queue, &event, 500 / portTICK_PERIOD_MS);
        if (pdFALSE == qres) {
            continue;
        }

        matrix_display_draw_rgb(rmt_items, display_buf, NUM_PIXELS);
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
    event_queue = xQueueCreate(2,1);

    // Init RMT
    rmt_config(&(rmt_config_t){
            .channel = RMT_CHANNEL,
            .gpio_num = GPIO_PIN,
            .clk_div = RMT_CLOCK_DIV, // clock source is 80MHz. Divide it to something useful
            .mem_block_num = 1,
            .rmt_mode = RMT_MODE_TX,
            .tx_config = {
                .loop_en = false,
                .carrier_freq_hz = 100, // not used, but has to be set to avoid divide by 0 err
                .carrier_duty_percent = 50,
                .carrier_level = RMT_CARRIER_LEVEL_LOW,
                .carrier_en = false,
                .idle_level = RMT_IDLE_LEVEL_LOW,
                .idle_output_en = true,

            }
    });
    rmt_driver_install(RMT_CHANNEL, 0, 0);

    xTaskCreatePinnedToCore(
        led_task,
        "ledtask",
        2096,
        NULL,
        1,
        NULL,
        0
    );

    xTaskCreatePinnedToCore(
        nats_task,
        "ledtask",
        4096,
        NULL,
        1,
        NULL,
        1
    );

    wifi_init_sta();
}
