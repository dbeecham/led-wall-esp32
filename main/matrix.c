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

#define NUM_PIXELS 50

static EventGroupHandle_t s_wifi_event_group;

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

struct matrix_hsv_s {
    uint8_t h;
    uint8_t s;
    uint8_t v;
};

struct matrix_hsv_s display_buf[NUM_PIXELS] = {0};
rmt_item32_t rmt_items[24*NUM_PIXELS] = {0};


struct matrix_rgb_s hsv_to_rgb (
    struct matrix_hsv_s hsv
)
{
    struct matrix_rgb_s rgb;
    uint8_t region, remainder, p, q, t;

    // No saturation, just return the value.
    if (0 == hsv.s) {
        rgb.r = hsv.v;
        rgb.g = hsv.v;
        rgb.b = hsv.v;
        return rgb;
    }

    region = hsv.h / 43;
    remainder = (hsv.h - (region * 43)) * 6;
    p = (hsv.v * (255 - hsv.s)) >> 8;
    q = (hsv.v * (255 - ((hsv.s * remainder) >> 8))) >> 8;
    t = (hsv.v * (255 - ((hsv.s * (255 - remainder)) >> 8))) >> 8;

    switch (region) {
        case 0:
            rgb.r = hsv.v;
            rgb.g = t;
            rgb.b = p;
            return rgb;

        case 1:
            rgb.r = q;
            rgb.g = hsv.v;
            rgb.b = p;
            return rgb;

        case 2:
            rgb.r = p;
            rgb.g = hsv.v;
            rgb.b = t;
            return rgb;

        case 3:
            rgb.r = p;
            rgb.g = q;
            rgb.b = hsv.v;
            return rgb;

        case 4:
            rgb.r = t;
            rgb.g = p;
            rgb.b = hsv.v;
            return rgb;

        default:
            rgb.r = hsv.v;
            rgb.g = p;
            rgb.b = q;
            return rgb;
    }
    

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
    struct matrix_hsv_s * buf,
    uint32_t buf_len
)
{

    uint32_t rmt_i = 0;
    uint8_t bit;
    struct matrix_rgb_s rgb;
    
    // For each pixel...
    for (int i = 0; i < buf_len; i++) {

        rgb = hsv_to_rgb(buf[i]);

        // Convert this pixels display buffer red to rmt_items.
        // For each bit in the buf[i].r, set the corresponding 
        for (bit = 8; bit > 0; bit--) {
            if ((rgb.r >> (bit - 1)) & 1) {
                items[rmt_i] = one;
            } else {
                items[rmt_i] = zero;
            }
            rmt_i += 1;
        }

        // Same thing with green
        for (bit = 8; bit > 0; bit--) {
            if ((rgb.g >> (bit - 1)) & 1) {
                items[rmt_i] = one;
            } else {
                items[rmt_i] = zero;
            }
            rmt_i += 1;
        }

        // And blue
        for (bit = 8; bit > 0; bit--) {
            if ((rgb.b >> (bit - 1)) & 1) {
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


static void led_task (
    void * arg
)
{

    for (int i = 0; i < NUM_PIXELS; i++) {
        display_buf[i].h = 0;
        display_buf[i].s = 180;
        display_buf[i].v = 90;
    }

    while(1) {

        for (int i = 0; i < NUM_PIXELS; i++) {
            display_buf[i].h += 1;
        }

        matrix_display_draw_rgb(rmt_items, display_buf, NUM_PIXELS);
        vTaskDelay(LED_STRIP_REFRESH_PERIOD_MS / portTICK_PERIOD_MS);
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

    wifi_init_sta();
}
