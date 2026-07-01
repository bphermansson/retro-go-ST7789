#include "rg_system.h"
#include "rg_display.h"

#include <stdlib.h>
#include <string.h>

#define LCD_BUFFER_LENGTH (RG_SCREEN_WIDTH * 4) // In pixels

// static rg_display_driver_t driver;
static rg_task_t *display_task_queue;
static rg_display_counters_t counters;
static rg_display_config_t config;
static rg_surface_t *border;
static rg_display_t display;
static int16_t map_viewport_to_source_x[RG_SCREEN_WIDTH + 1];
static int16_t map_viewport_to_source_y[RG_SCREEN_HEIGHT + 1];
static uint32_t screen_line_checksum[RG_SCREEN_HEIGHT + 1];

#define LINE_IS_REPEATED(Y) (map_viewport_to_source_y[(Y)] == map_viewport_to_source_y[(Y) - 1])
// This is to avoid flooring a number that is approximated to .9999999 and be explicit about it
#define FLOAT_TO_INT(x) ((int)((x) + 0.1f))

static const char *SETTING_BACKLIGHT = "DispBacklight";
static const char *SETTING_SCALING = "DispScaling";
static const char *SETTING_FILTER = "DispFilter";
static const char *SETTING_ROTATION = "DispRotation";
static const char *SETTING_BORDER = "DispBorder";
static const char *SETTING_CUSTOM_ZOOM = "DispCustomZoom";

static void lcd_init(void);
static void lcd_deinit(void);
static void lcd_sync(void);
static void lcd_set_rotation(int rotation);
static void lcd_set_backlight(float percent);
static void lcd_set_window(int left, int top, int width, int height);
static inline uint16_t *lcd_get_buffer(size_t length);
static inline void lcd_send_buffer(uint16_t *buffer, size_t length);

<<<<<<< HEAD
#if RG_SCREEN_DRIVER == 0 || RG_SCREEN_DRIVER == 1 /* ILI9341/ST7789 */
#include "drivers/display/ili9341.h"
#elif RG_SCREEN_DRIVER == 99
#include "drivers/display/sdl2.h"
=======

static inline uint16_t *spi_get_buffer(void)
{
    uint16_t *buffer;
    if (xQueueReceive(spi_buffers, &buffer, pdMS_TO_TICKS(2500)) != pdTRUE)
        RG_PANIC("display");
    return buffer;
}

static inline void spi_queue_transaction(const void *data, size_t length, uint32_t type)
{
    spi_transaction_t *t;

    if (!data || length < 1)
        return;

    xQueueReceive(spi_transactions, &t, portMAX_DELAY);

    *t = (spi_transaction_t){
        .tx_buffer = NULL,
        .length = length * 8, // In bits
        .user = (void *)type,
        .flags = 0,
    };

    if (type & 2)
    {
        t->tx_buffer = data;
    }
    else if (length < 5)
    {
        memcpy(t->tx_data, data, length);
        t->flags = SPI_TRANS_USE_TXDATA;
    }
    else
    {
        t->tx_buffer = memcpy(spi_get_buffer(), data, length);
    }

    if (spi_device_queue_trans(spi_dev, t, pdMS_TO_TICKS(2500)) != ESP_OK)
    {
        RG_PANIC("display");
    }
}

IRAM_ATTR
static void spi_pre_transfer_cb(spi_transaction_t *t)
{
    // Set the data/command line accordingly
    gpio_set_level(RG_GPIO_LCD_DC, (int)t->user & 1);
}

IRAM_ATTR
static void spi_task(void *arg)
{
    spi_transaction_t *t;

    while (spi_device_get_trans_result(spi_dev, &t, portMAX_DELAY) == ESP_OK)
    {
        if ((int)t->user & 2)
            xQueueSend(spi_buffers, &t->tx_buffer, 0);
        xQueueSend(spi_transactions, &t, 0);
    }

    // This should never happen
    rg_task_delete(NULL);
}

static void spi_init(void)
{
    spi_transactions = xQueueCreate(SPI_TRANSACTION_COUNT, sizeof(spi_transaction_t *));
    spi_buffers = xQueueCreate(SPI_BUFFER_COUNT, sizeof(uint16_t *));

    while (uxQueueSpacesAvailable(spi_transactions))
    {
        void *trans = malloc(sizeof(spi_transaction_t));
        xQueueSend(spi_transactions, &trans, portMAX_DELAY);
    }

    while (uxQueueSpacesAvailable(spi_buffers))
    {
        void *buffer = rg_alloc(SPI_BUFFER_LENGTH * 2, MEM_DMA);
        xQueueSend(spi_buffers, &buffer, portMAX_DELAY);
    }

    const spi_bus_config_t buscfg = {
        .miso_io_num = RG_GPIO_LCD_MISO,
        .mosi_io_num = RG_GPIO_LCD_MOSI,
        .sclk_io_num = RG_GPIO_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = RG_SCREEN_SPEED,   // Typically SPI_MASTER_FREQ_40M or SPI_MASTER_FREQ_80M
        .mode = 0,                           // SPI mode 0
        .spics_io_num = RG_GPIO_LCD_CS,      // CS pin
        .queue_size = SPI_TRANSACTION_COUNT, // We want to be able to queue 5 transactions at a time
        .pre_cb = &spi_pre_transfer_cb,      // Specify pre-transfer callback to handle D/C line and SPI lock
        .flags = SPI_DEVICE_NO_DUMMY,        // SPI_DEVICE_HALFDUPLEX;
    };

    esp_err_t ret;

    // Initialize the SPI bus
    ret = spi_bus_initialize(RG_SCREEN_HOST, &buscfg, SPI_DMA_CH_AUTO);
    RG_ASSERT(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE, "spi_bus_initialize failed.");

    ret = spi_bus_add_device(RG_SCREEN_HOST, &devcfg, &spi_dev);
    RG_ASSERT(ret == ESP_OK, "spi_bus_add_device failed.");

    rg_task_create("rg_spi", &spi_task, NULL, 1.5 * 1024, RG_TASK_PRIORITY - 1, 1);
}

static void spi_deinit(void)
{
    spi_bus_remove_device(spi_dev);
    spi_bus_free(RG_SCREEN_HOST);
}

static void ili9341_cmd(uint8_t cmd, const void *data, size_t data_len)
{
    spi_queue_transaction(&cmd, 1, 0);
    if (data && data_len > 0)
        spi_queue_transaction(data, data_len, 1);
    // if ((cmd & 0xE0) == 0x00)
    //     usleep(5000);
}

static void lcd_set_backlight(double percent)
{
    double level = RG_MIN(RG_MAX(percent / 100.0, 0), 1.0);
    int error_code = 0;

#if defined(RG_GPIO_LCD_BCKL)
    error_code = ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0x1FFF * level, 50, 0);
#elif defined(RG_TARGET_QTPY_GAMER)
    // error_code = aw_analogWrite(AW_TFT_BACKLIGHT, level * 255);
#endif

    if (error_code)
        RG_LOGE("failed setting backlight to %.2f%% (0x%02X)\n", 100 * level, error_code);
    else
        RG_LOGI("backlight set to %.2f%%\n", 100 * level);
}

static void lcd_init(void)
{
#if defined(RG_GPIO_LCD_BCKL)
    // Initialize backlight at 0% to avoid the lcd reset flash
    ledc_timer_config(&(ledc_timer_config_t){
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 5000,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
    });
    ledc_channel_config(&(ledc_channel_config_t){
        .channel = LEDC_CHANNEL_0,
        .duty = 0,
        .gpio_num = RG_GPIO_LCD_BCKL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LEDC_TIMER_0,
    });
    ledc_fade_func_install(0);
#endif

    spi_init();

    // Setup Data/Command line
    gpio_set_direction(RG_GPIO_LCD_DC, GPIO_MODE_OUTPUT);
    gpio_set_level(RG_GPIO_LCD_DC, 1);

#if defined(RG_GPIO_LCD_RST)
    gpio_set_direction(RG_GPIO_LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(RG_GPIO_LCD_RST, 0);
    usleep(100 * 1000);
    gpio_set_level(RG_GPIO_LCD_RST, 1);
    usleep(10 * 1000);
#endif

#define ILI9341_CMD(cmd, data...) {const uint8_t x[] = data; ili9341_cmd(cmd, x, sizeof(x));}
#if RG_SCREEN_TYPE == 0 // LCD Model (ODROID-GO)

ILI9341_CMD(0x01, {});     // Reset
    ILI9341_CMD(0x3A, {0x55}); // Pixel Format Set RGB565
    ILI9341_CMD(0xCF, {0x00, 0xc3, 0x30});
    ILI9341_CMD(0xED, {0x64, 0x03, 0x12, 0x81});
    ILI9341_CMD(0xE8, {0x85, 0x00, 0x78});
    ILI9341_CMD(0xCB, {0x39, 0x2c, 0x00, 0x34, 0x02});
    ILI9341_CMD(0xF7, {0x20});
    ILI9341_CMD(0xEA, {0x00, 0x00});
    ILI9341_CMD(0xC0, {0x1B});                                  // Power control   //VRH[5:0]
    ILI9341_CMD(0xC1, {0x12});                                  // Power control   //SAP[2:0];BT[3:0]
    ILI9341_CMD(0xC5, {0x32, 0x3C});                            // VCM control
    ILI9341_CMD(0xC7, {0x91});                                  // VCM control2
    ILI9341_CMD(0x36, {(0x00|0x00|0x08)});                      // Memory Access Control
    ILI9341_CMD(0xB1, {0x00, 0x10});                            // Frame Rate Control (1B=70, 1F=61, 10=119)
    ILI9341_CMD(0xB6, {0x0A, 0xA2});                            // Display Function Control
    ILI9341_CMD(0xF6, {0x01, 0x30});
    ILI9341_CMD(0xF2, {0x00});                                  // 3Gamma Function Disable
    ILI9341_CMD(0x26, {0x01});                                  // Gamma curve selected
    
    //ILI9341_CMD(0xE0, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}); // Set Gamma
    //ILI9341_CMD(0xE1, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}); // Set Gamma
    ILI9341_CMD(0xE0, {0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43, 0x47, 0x09, 0x15, 0x12, 0x16, 0x19}); // Set Gamma
    ILI9341_CMD(0xE1, {0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44, 0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19}); // Set Gamma
    ILI9341_CMD(0x11, {}); // Exit Sleep
    ILI9341_CMD(0x29, {}); // Display on
 

/*
    ILI9341_CMD(0x01, {});     // Reset
    ILI9341_CMD(0x13, {});     // NORON
    
    ILI9341_CMD(0xCF, {0x00, 0xc3, 0x30});
    ILI9341_CMD(0xED, {0x64, 0x03, 0x12, 0x81});
    ILI9341_CMD(0xE8, {0x85, 0x00, 0x78});
    ILI9341_CMD(0xCB, {0x39, 0x2c, 0x00, 0x34, 0x02});
    ILI9341_CMD(0xF7, {0x20});
    ILI9341_CMD(0xEA, {0x00, 0x00});
    ILI9341_CMD(0xC0, {0x1B});                                  // Power control   //VRH[5:0]
    ILI9341_CMD(0xC1, {0x12});                                  // Power control   //SAP[2:0];BT[3:0]
    ILI9341_CMD(0xC5, {0x32, 0x3C});                            // VCM control
    ILI9341_CMD(0xC7, {0x91});                                  // VCM control2
    ILI9341_CMD(0x36, {(0x00|0x00|0x08)});                      // Memory Access Control
    
    ILI9341_CMD(0x3A, {0x55}); // Pixel Format Set RGB565
    ILI9341_CMD(0x0c, {0x0c, 0x00, 0x33, 0x33});
    ILI9341_CMD(0xB7, {0x72});
    ILI9341_CMD(0xBB, {0x3d});
    ILI9341_CMD(0xC0, {0x2C});                                  // Power control 
    ILI9341_CMD(0xC2, {0x01, 0xFF});
    ILI9341_CMD(0xC3, {0x19});
    ILI9341_CMD(0xC4, {0x20});
    ILI9341_CMD(0xC6, {0x0f});
    ILI9341_CMD(0xD0, {0xA4, 0xA1});
    ILI9341_CMD(0xE0, {0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43, 0x47, 0x09, 0x15, 0x12, 0x16, 0x19}); // Set Gamma
    ILI9341_CMD(0xE1, {0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44, 0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19}); // Set Gamma
    ILI9341_CMD(0x21, {0x80});   
    ILI9341_CMD(0x11, {0x80}); // Exit Sleep
    ILI9341_CMD(0x29, {0x80}); // Display on
    
    ILI9341_CMD(0x01, {});     // Reset
    ILI9341_CMD(0x13, {});     // NORON
    
    ILI9341_CMD(0xCF, {0x00, 0xc3, 0x30});
    ILI9341_CMD(0xED, {0x64, 0x03, 0x12, 0x81});
    ILI9341_CMD(0xE8, {0x85, 0x00, 0x78});
    ILI9341_CMD(0xCB, {0x39, 0x2c, 0x00, 0x34, 0x02});
    ILI9341_CMD(0xF7, {0x20});
    ILI9341_CMD(0xEA, {0x00, 0x00});
    ILI9341_CMD(0xC0, {0x1B});                                  // Power control   //VRH[5:0]
    ILI9341_CMD(0xC1, {0x12});                                  // Power control   //SAP[2:0];BT[3:0]
    ILI9341_CMD(0xC5, {0x32, 0x3C});                            // VCM control
    ILI9341_CMD(0xC7, {0x91});                                  // VCM control2
    ILI9341_CMD(0x36, {(0x00|0x00|0x08)});                      // Memory Access Control
    ILI9341_CMD(0xB1, {0x00, 0x10});                            // Frame Rate Control (1B=70, 1F=61, 10=119)
    ILI9341_CMD(0xB6, {0x0A, 0xA2});                            // Display Function Control
    ILI9341_CMD(0xF6, {0x01, 0x30});
    ILI9341_CMD(0xF2, {0x00});                                  // 3Gamma Function Disable
    ILI9341_CMD(0x26, {0x01});                                  // Gamma curve selected
    
    ILI9341_CMD(0xB0, {0x00, 0xE0});
    ILI9341_CMD(0x3A, {0x55}); 
    usleep(10 * 10);
    ILI9341_CMD(0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x03});
    ILI9341_CMD(0xB7, {0x35});
    ILI9341_CMD(0xBB, {0x28});
    //ILI9341_CMD(0xC0, {0x0C});
    ILI9341_CMD(0xC2, {0x01, 0xFF});
    ILI9341_CMD(0xC3, {0x10});
    ILI9341_CMD(0xC4, {0x20});
    ILI9341_CMD(0xC6, {0x0F});
    ILI9341_CMD(0xD0, {0xA4, 0xA1});
    //ILI9341_CMD(0xE0, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17}); // Set Gamma
    //ILI9341_CMD(0xE1, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E}); // Set Gamma
    
    ILI9341_CMD(0xE0, {0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43, 0x47, 0x09, 0x15, 0x12, 0x16, 0x19}); // Set Gamma
    ILI9341_CMD(0xE1, {0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44, 0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19}); // Set Gamma
    ILI9341_CMD(0x21, {});    
    ILI9341_CMD(0x2A, {0x00, 0x00, 0x00, 0xEF});
    ILI9341_CMD(0x2B, {0x00, 0x00, 0x01, 0x3F});
    ILI9341_CMD(0x11, {}); // Exit Sleep
    ILI9341_CMD(0x29, {}); // Display on
   
    
    ILI9341_CMD(0xCF, {0x00, 0xc3, 0x30});
    ILI9341_CMD(0xED, {0x64, 0x03, 0x12, 0x81});
    ILI9341_CMD(0xE8, {0x85, 0x00, 0x78});
    ILI9341_CMD(0xCB, {0x39, 0x2c, 0x00, 0x34, 0x02});
    ILI9341_CMD(0xF7, {0x20});
    ILI9341_CMD(0xEA, {0x00, 0x00});
    ILI9341_CMD(0xC0, {0x1B});                                  // Power control   //VRH[5:0]
    ILI9341_CMD(0xC1, {0x12});                                  // Power control   //SAP[2:0];BT[3:0]
    ILI9341_CMD(0xC5, {0x32, 0x3C});                            // VCM control
    ILI9341_CMD(0xC7, {0x91});                                  // VCM control2
    ILI9341_CMD(0x36, {(0x00|0x00|0x08)});                      // Memory Access Control
    ILI9341_CMD(0xB1, {0x00, 0x10});                            // Frame Rate Control (1B=70, 1F=61, 10=119)
    ILI9341_CMD(0xB6, {0x0A, 0xA2});                            // Display Function Control
    ILI9341_CMD(0xF6, {0x01, 0x30});
    ILI9341_CMD(0xF2, {0x00});                                  // 3Gamma Function Disable
    ILI9341_CMD(0x26, {0x01});                                  // Gamma curve selected
    ILI9341_CMD(0xE0, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}); // Set Gamma
    ILI9341_CMD(0xE1, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}); // Set Gamma
    ILI9341_CMD(0x11, {}); // Exit Sleep
    ILI9341_CMD(0x29, {}); // Display on
    */
#elif RG_SCREEN_TYPE == 1 // LCD Model (MRGC-G32)
    ILI9341_CMD(0x01, {});     // Reset
    ILI9341_CMD(0x3A, {0x55}); // Pixel Format Set RGB565
    ILI9341_CMD(0x36, {(0x00|0x00|0x00)});
    ILI9341_CMD(0xB1, {0x00, 0x10});                            // Frame Rate Control (1B=70, 1F=61, 10=119)
    ILI9341_CMD(0xB2, {0x0c, 0x0c, 0x00, 0x33, 0x33});
    ILI9341_CMD(0xB7, {0x35});
    ILI9341_CMD(0xBB, {0x24});
    ILI9341_CMD(0xC0, {0x2C});
    ILI9341_CMD(0xC2, {0x01, 0xFF});
    ILI9341_CMD(0xC3, {0x11});
    ILI9341_CMD(0xC4, {0x20});
    ILI9341_CMD(0xC6, {0x0f});
    ILI9341_CMD(0xD0, {0xA4, 0xA1});
    ILI9341_CMD(0xE0, {0xD0, 0x00, 0x03, 0x09, 0x13, 0x1C, 0x3A, 0x55, 0x48, 0x18, 0x12, 0x0E, 0x19, 0x1E});
    ILI9341_CMD(0xE1, {0xD0, 0x00, 0x03, 0x09, 0x05, 0x25, 0x3A, 0x55, 0x50, 0x3D, 0x1C, 0x1D, 0x1D, 0x1E});
    ILI9341_CMD(0x11, {}); // Exit Sleep
    ILI9341_CMD(0x29, {}); // Display on
#elif RG_SCREEN_TYPE == 2 // LCD Model (QT-PY Gamer)
    ILI9341_CMD(0x01, {});     // Reset
    ILI9341_CMD(0x11, {}); // Exit Sleep
    ILI9341_CMD(0x3A, {0x55}); // Pixel Format Set RGB565
    ILI9341_CMD(0x36, {0xC0});
    ILI9341_CMD(0x2A, {0, 0, 0, 240}); // CASET
    ILI9341_CMD(0x2B, {0, 0, 320>>8, 320&0xFF}); // RASET
    ILI9341_CMD(0x21, {});
    ILI9341_CMD(0x29, {}); // Display on
#elif RG_SCREEN_TYPE == 32 // LCD Model (Retro-ESP32)
    ILI9341_CMD(0x01, {});     // Reset
    ILI9341_CMD(0x3A, {0x55}); // Pixel Format Set RGB565
    ILI9341_CMD(0xCF, {0x00, 0xc3, 0x30});
    ILI9341_CMD(0xED, {0x64, 0x03, 0x12, 0x81});
    ILI9341_CMD(0xE8, {0x85, 0x00, 0x78});
    ILI9341_CMD(0xCB, {0x39, 0x2c, 0x00, 0x34, 0x02});
    ILI9341_CMD(0xF7, {0x20});
    ILI9341_CMD(0xEA, {0x00, 0x00});
    ILI9341_CMD(0xC0, {0x1B});                                  // Power control   //VRH[5:0]
    ILI9341_CMD(0xC1, {0x12});                                  // Power control   //SAP[2:0];BT[3:0]
    ILI9341_CMD(0xC5, {0x32, 0x3C});                            // VCM control
    ILI9341_CMD(0xC7, {0x91});                                  // VCM control2
    ILI9341_CMD(0x36, {(0x20|0x80|0x08)});                      // Memory Access Control
    ILI9341_CMD(0x36, {(0x40|0x80|0x08)});                      // Memory Access Control
    //ILI9341_CMD(0x21, {0x80});                                  // invert colors
    ILI9341_CMD(0xB1, {0x00, 0x10});                            // Frame Rate Control (1B=70, 1F=61, 10=119)
    ILI9341_CMD(0xB6, {0x0A, 0xA2});                            // Display Function Control
    ILI9341_CMD(0xF6, {0x01, 0x30});
    ILI9341_CMD(0xF2, {0x00});                                  // 3Gamma Function Disable
    ILI9341_CMD(0x26, {0x01});                                  // Gamma curve selected
    ILI9341_CMD(0xE0, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}); // Set Gamma
    ILI9341_CMD(0xE1, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}); // Set Gamma
    ILI9341_CMD(0x11, {}); // Exit Sleep
    ILI9341_CMD(0x29, {}); // Display on
#elif RG_SCREEN_TYPE == 4
	ILI9341_CMD(0x01, {});  // Reset
	usleep(120 * 1000);
	ILI9341_CMD(0x3A, {0X05});  //65k mode
	ILI9341_CMD(0xC5, {0x1A}); //VCOM
	ILI9341_CMD(0x36, {0x60}); //Display Rotation
	ILI9341_CMD(0xB2, {0x05, 0x05, 0x00, 0x33, 0x33});  //Porch Setting
	ILI9341_CMD(0xB7, {0x05});  //Gate Control //12.2v   -10.43v
	ILI9341_CMD(0xBB, {0x3F});  //VCOM
	ILI9341_CMD(0xC0, {0x2c});  //Power control
	ILI9341_CMD(0xC2, {0x01});  //VDV and VRH Command Enable
	ILI9341_CMD(0xC3, {0x0F});  //VRH Set 4.3+( vcom+vcom offset+vdv)
	ILI9341_CMD(0xC4, {0xBE});  //VDV Set 0v
	ILI9341_CMD(0xC6, {0X01});  //Frame Rate Control in Normal Mode 111Hz
	ILI9341_CMD(0xD0, {0xA4,0xA1});  //Power Control 1
	ILI9341_CMD(0xE8, {0x03});   //Power Control 1
	ILI9341_CMD(0xE9, {0x09,0x09,0x08});  //Equalize time control
	ILI9341_CMD(0xE0, {0xD0,0x05,0x09,0x09,0x08,0x14,0x28,0x33,0x3F,0x07,0x13,0x14,0x28,0x30});   //Set Gamma
	ILI9341_CMD(0xE1, {0xD0, 0x05, 0x09, 0x09, 0x08, 0x03, 0x24, 0x32, 0x32, 0x3B, 0x14, 0x13, 0x28, 0x2F, 0x1F});   //Set Gamma
	ILI9341_CMD(0x20, {0x00});   //Reverse Display
	ILI9341_CMD(0x11, {0x03});   //Exit Sleep
	ILI9341_CMD(0x29, {0x03});   //Display on
	usleep(100 * 1000);
#elif RG_SCREEN_TYPE == 5 // Game Box Mini Screen
    ILI9341_CMD(0x3A, {0x55}); // Pixel Format Set RGB565
    ILI9341_CMD(0x0c, {0x0c, 0x00, 0x33, 0x33});
    ILI9341_CMD(0xB7, {0x72});
    ILI9341_CMD(0xBB, {0x3d});
    ILI9341_CMD(0xC0, {0x2C});                                  // Power control 
    ILI9341_CMD(0xC2, {0x01, 0xFF});
    ILI9341_CMD(0xC3, {0x19});
    ILI9341_CMD(0xC4, {0x20});
    ILI9341_CMD(0xC6, {0x0f});
    ILI9341_CMD(0xD0, {0xA4, 0xA1});
    ILI9341_CMD(0xE0, {0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43, 0x47, 0x09, 0x15, 0x12, 0x16, 0x19}); // Set Gamma
    ILI9341_CMD(0xE1, {0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44, 0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19}); // Set Gamma
    ILI9341_CMD(0x21, {0x80});   
    ILI9341_CMD(0x11, {0x80}); // Exit Sleep
    ILI9341_CMD(0x29, {0x80}); // Display on
>>>>>>> 2493140b182650d5336d24bdda1356a799aa14a8
#else
#include "drivers/display/dummy.h"
#endif

static int draw_on_screen_display(int region_start, int region_end)
{
    static unsigned int area_dirty = 0;
    rg_margins_t margins = rg_gui_get_safe_area();
    int left = display.screen.width - margins.right - 28;
    int top = margins.top + 4;
    int border = 3;
    int width = 20;
    int height = 14;

    if (region_end < top + height)
        return top + height;

    // Low battery indicator
    if (rg_system_get_indicator(RG_INDICATOR_POWER_LOW) && ((counters.totalFrames / 20) & 1))
    {
        rg_display_clear_rect(left, top, width, height, C_RED); // Main body
        rg_display_clear_rect(left + width, top + height / 4, border, height / 2, C_RED); // The tab
        rg_display_clear_rect(left + border, top + border, width - border * 2, height - border * 2, C_BLACK); // The fill
        // memset(&screen_line_checksum[top], 0, sizeof(uint32_t) * height);
        area_dirty |= (1 << RG_INDICATOR_POWER_LOW);
    }
    else if (area_dirty)
    {
        if (display.viewport.width < display.screen.width || display.viewport.height < display.screen.height)
            rg_display_clear_rect(left, top, width + border, height, C_BLACK);
        memset(&screen_line_checksum[top], 0, sizeof(uint32_t) * height);
        area_dirty = 0;
    }
    return 0;
}

static inline unsigned blend_pixels(unsigned a, unsigned b)
{
    // Fast path (taken 80-90% of the time)
    if (a == b)
        return a;

    // Not the original author, but a good explanation is found at:
    // https://medium.com/@luc.trudeau/fast-averaging-of-high-color-16-bit-pixels-cb4ac7fd1488
    a = (a << 8) | (a >> 8);
    b = (b << 8) | (b >> 8);
    unsigned s = a ^ b;
    unsigned v = ((s & 0xF7DEU) >> 1) + (a & b) + (s & 0x0821U);
    return (v << 8) | (v >> 8);

    // This is my attempt at averaging two 565BE values without swapping bytes (3x the speed of the code above)
    // return (((a ^ b) & 0b1101111011110110U) >> 1) + (a & b);
}

static inline void write_update(const rg_surface_t *update)
{
    const int64_t time_start = rg_system_timer();

    bool filter_x = display.viewport.filter_x;
    bool filter_y = display.viewport.filter_y;
    int draw_left = display.viewport.left;
    int draw_top = display.viewport.top;
    int draw_width = display.viewport.width;
    int draw_height = display.viewport.height;

    int crop_left = 0;
    int crop_top = 0;

    if (draw_left < 0)
    {
        crop_left += -draw_left * display.viewport.step_x;
        draw_width += draw_left * 2;
        draw_left = 0;
    }

    if (draw_top < 0)
    {
        crop_top += -draw_top * display.viewport.step_y;
        draw_height += draw_top * 2;
        draw_top = 0;
    }

    const int format = update->format;
    const int stride = update->stride;
    const void *data = update->data + update->offset + (crop_top * stride) + (crop_left * RG_PIXEL_GET_SIZE(format));
    const uint16_t *palette = update->palette;

    const bool partial_update = RG_SCREEN_PARTIAL_UPDATES;

    int lines_per_buffer = LCD_BUFFER_LENGTH / draw_width;
    int lines_remaining = draw_height;
    int lines_updated = 0;
    int window_top = -1;
    int osd_next_call = 20;

    for (int y = 0; y < draw_height;)
    {
        int lines_to_copy = RG_MIN(lines_per_buffer, lines_remaining);

        if (lines_to_copy < 1)
            break;

        // The vertical filter requires a block to start and end with unscaled lines
        if (filter_y)
        {
            while (lines_to_copy > 1 && (LINE_IS_REPEATED(y + lines_to_copy - 1) ||
                                         LINE_IS_REPEATED(y + lines_to_copy)))
                --lines_to_copy;
        }

        uint16_t *line_buffer = lcd_get_buffer(LCD_BUFFER_LENGTH);
        uint16_t *line_buffer_ptr = line_buffer;

        uint32_t checksum = 0xFFFFFFFF;
        bool need_update = !partial_update;

        for (int i = 0; i < lines_to_copy; ++i)
        {
            if (i > 0 && LINE_IS_REPEATED(y))
            {
                memcpy(line_buffer_ptr, line_buffer_ptr - draw_width, draw_width * 2);
                line_buffer_ptr += draw_width;
            }
            else
            {
                #define RENDER_LINE(PTR_TYPE, PIXEL) { \
                    PTR_TYPE *buffer = (PTR_TYPE *)(data + map_viewport_to_source_y[y] * stride);\
                    for (int xx = 0; xx < draw_width; ++xx) { \
                        int x = map_viewport_to_source_x[xx]; \
                        *line_buffer_ptr++ = (PIXEL); \
                    } \
                }
                if (format & RG_PIXEL_PALETTE)
                    RENDER_LINE(uint8_t, palette[buffer[x]])
                else if (format == RG_PIXEL_565_LE)
                    RENDER_LINE(uint16_t, (buffer[x] << 8) | (buffer[x] >> 8))
                else
                    RENDER_LINE(uint16_t, buffer[x])

                if (partial_update)
                {
                    checksum = rg_hash((void*)(line_buffer_ptr - draw_width), draw_width * 2);
                }
            }

            if (screen_line_checksum[draw_top + y] != checksum)
            {
                screen_line_checksum[draw_top + y] = checksum;
                need_update = true;
            }

            ++y;
        }

        if (filter_x && need_update)
        {
            for (int i = 0; i < lines_to_copy; ++i)
            {
                uint16_t *buffer = line_buffer + i * draw_width;
                for (int x = 1; x < draw_width - 1; ++x)
                {
                    if (map_viewport_to_source_x[x] == map_viewport_to_source_x[x - 1])
                    {
                        buffer[x] = blend_pixels(buffer[x - 1], buffer[x + 1]);
                    }
                }
            }
        }

        if (filter_y && need_update)
        {
            int top = y - lines_to_copy;
            for (int i = 1; i < lines_to_copy - 1; ++i)
            {
                if (LINE_IS_REPEATED(top + i))
                {
                    uint16_t *lineA = line_buffer + (i - 1) * draw_width;
                    uint16_t *lineB = line_buffer + (i + 0) * draw_width;
                    uint16_t *lineC = line_buffer + (i + 1) * draw_width;
                    for (size_t x = 0; x < draw_width; ++x)
                    {
                        lineB[x] = blend_pixels(lineA[x], lineC[x]);
                    }
                }
            }
        }

        if (need_update)
        {
            int left = display.screen.margins.left + draw_left;
            int top = display.screen.margins.top + draw_top + y - lines_to_copy;
            if (top != window_top)
                lcd_set_window(left, top, draw_width, lines_remaining);
            lcd_send_buffer(line_buffer, draw_width * lines_to_copy);
            window_top = top + lines_to_copy;
            lines_updated += lines_to_copy;
        }
        else
        {
            // Return unused buffer
            lcd_send_buffer(line_buffer, 0);
        }

        // Drawing the OSD as we progress reduces flicker compared to doing it once at the end
        if (osd_next_call && draw_top + y >= osd_next_call)
        {
            osd_next_call = draw_on_screen_display(0, draw_top + y);
            window_top = -1;
        }

        lines_remaining -= lines_to_copy;
    }

    if (lines_updated > draw_height * 0.80f)
        counters.fullFrames++;
    else
        counters.partFrames++;
    counters.busyTime += rg_system_timer() - time_start;
}

static void update_viewport_scaling(void)
{
    int screen_width = display.screen.width;
    int screen_height = display.screen.height;
    int src_width = display.source.width;
    int src_height = display.source.height;
    int new_width = src_width;
    int new_height = src_height;

    if (config.scaling == RG_DISPLAY_SCALING_FULL)
    {
        new_width = screen_width;
        new_height = screen_height;
    }
    else if (config.scaling == RG_DISPLAY_SCALING_FIT)
    {
        new_width = FLOAT_TO_INT(screen_height * ((float)src_width / src_height));
        new_height = screen_height;
        if (new_width > screen_width) {
            new_width = screen_width;
            new_height = FLOAT_TO_INT(screen_width * ((float)src_height / src_width));
        }
    }
    else if (config.scaling == RG_DISPLAY_SCALING_ZOOM)
    {
        new_width = FLOAT_TO_INT(src_width * config.custom_zoom);
        new_height = FLOAT_TO_INT(src_height * config.custom_zoom);
    }

    // Everything works better when we use even dimensions!
    new_width &= ~1;
    new_height &= ~1;

    display.viewport.left = (screen_width - new_width) / 2;
    display.viewport.top = (screen_height - new_height) / 2;
    display.viewport.width = new_width;
    display.viewport.height = new_height;

    display.viewport.step_x = (float)src_width / display.viewport.width;
    display.viewport.step_y = (float)src_height / display.viewport.height;

    display.viewport.filter_x = (config.filter == RG_DISPLAY_FILTER_HORIZ || config.filter == RG_DISPLAY_FILTER_BOTH) &&
                                (config.scaling && (display.viewport.width % src_width) != 0);
    display.viewport.filter_y = (config.filter == RG_DISPLAY_FILTER_VERT || config.filter == RG_DISPLAY_FILTER_BOTH) &&
                                (config.scaling && (display.viewport.height % src_height) != 0);

    memset(screen_line_checksum, 0, sizeof(screen_line_checksum));

    for (int x = 0; x < screen_width; ++x)
        map_viewport_to_source_x[x] = FLOAT_TO_INT(x * display.viewport.step_x);
    for (int y = 0; y < screen_height; ++y)
        map_viewport_to_source_y[y] = FLOAT_TO_INT(y * display.viewport.step_y);

    RG_LOGI("%dx%d@%.3f => %dx%d@%.3f left:%d top:%d step_x:%.2f step_y:%.2f", src_width, src_height,
            (float)src_width / src_height, new_width, new_height, (float)new_width / new_height,
            display.viewport.left, display.viewport.top, display.viewport.step_x, display.viewport.step_y);
}

static bool load_border_file(const char *filename)
{
    RG_LOGI("Loading border file: %s", filename ?: "(none)");

    free(border), border = NULL;
    display.changed = true;

    if (filename && (border = rg_surface_load_image_file(filename, 0)))
    {
        if (border->width != rg_display_get_width() || border->height != rg_display_get_height())
        {
            rg_surface_t *resized = rg_surface_resize(border, rg_display_get_width(), rg_display_get_height());
            if (resized)
            {
                rg_surface_free(border);
                border = resized;
            }
        }
        return true;
    }
    return false;
}

IRAM_ATTR
static void display_task(void *arg)
{
    rg_task_msg_t msg;

    while (rg_task_peek(&msg))
    {
        // Received a shutdown request!
        if (msg.type == RG_TASK_MSG_STOP)
            break;

        if (display.changed)
        {
            update_viewport_scaling();
            // Clear the screen if the viewport doesn't cover the entire screen because garbage could remain on the sides
            if (display.viewport.width < display.screen.width || display.viewport.height < display.screen.height)
            {
                if (border)
                    rg_display_write_rect(0, 0, border->width, border->height, 0, border->data, RG_DISPLAY_WRITE_NOSYNC);
                else
                    rg_display_clear_except(display.viewport.left, display.viewport.top, display.viewport.width, display.viewport.height, C_BLACK);
            }
            display.changed = false;
        }

        write_update(msg.dataPtr);
        // draw_on_screen_display(0, display.screen.height);
        rg_task_receive(&msg);

        lcd_sync();
    }
}

void rg_display_force_redraw(void)
{
    display.changed = true;
    // memset(screen_line_checksum, 0, sizeof(screen_line_checksum));
    rg_system_event(RG_EVENT_REDRAW, NULL);
    rg_display_sync(true);
}

const rg_display_t *rg_display_get_info(void)
{
    return &display;
}

rg_display_counters_t rg_display_get_counters(void)
{
    return counters;
}

int rg_display_get_width(void)
{
    // return display.screen.real_width - (display.screen.margins.left + display.screen.margins.right);
    return display.screen.width;
}

int rg_display_get_height(void)
{
    // return display.screen.real_height - (display.screen.margins.top + display.screen.margins.bottom);
    return display.screen.height;
}

void rg_display_set_scaling(display_scaling_t scaling)
{
    config.scaling = RG_MIN(RG_MAX(0, scaling), RG_DISPLAY_SCALING_COUNT - 1);
    rg_settings_set_number(NS_APP, SETTING_SCALING, config.scaling);
    display.changed = true;
}

display_scaling_t rg_display_get_scaling(void)
{
    return config.scaling;
}

void rg_display_set_custom_zoom(double factor)
{
    config.custom_zoom = RG_MIN(RG_MAX(0.1, factor), 2.0);
    rg_settings_set_number(NS_APP, SETTING_CUSTOM_ZOOM, config.custom_zoom);
    display.changed = true;
}

double rg_display_get_custom_zoom(void)
{
    return config.custom_zoom;
}

void rg_display_set_filter(display_filter_t filter)
{
    config.filter = RG_MIN(RG_MAX(0, filter), RG_DISPLAY_FILTER_COUNT - 1);
    rg_settings_set_number(NS_APP, SETTING_FILTER, config.filter);
    display.changed = true;
}

display_filter_t rg_display_get_filter(void)
{
    return config.filter;
}

void rg_display_set_rotation(display_rotation_t rotation)
{
    config.rotation = RG_MIN(RG_MAX(0, rotation), RG_DISPLAY_ROTATION_COUNT - 1);
    rg_settings_set_number(NS_APP, SETTING_SCALING, config.rotation);
    display.changed = true;
}

display_rotation_t rg_display_get_rotation(void)
{
    return config.rotation;
}

void rg_display_set_backlight(display_backlight_t percent)
{
    config.backlight = RG_MIN(RG_MAX(percent, RG_DISPLAY_BACKLIGHT_MIN), RG_DISPLAY_BACKLIGHT_MAX);
    rg_settings_set_number(NS_GLOBAL, SETTING_BACKLIGHT, config.backlight);
    lcd_set_backlight(config.backlight);
}

display_backlight_t rg_display_get_backlight(void)
{
    return config.backlight;
}

void rg_display_set_border(const char *filename)
{
    free(config.border_file);
    config.border_file = NULL;

    if (load_border_file(filename))
    {
        rg_settings_set_string(NS_APP, SETTING_BORDER, filename);
        config.border_file = strdup(filename);
    }
    else
    {
        rg_settings_set_string(NS_APP, SETTING_BORDER, NULL);
        config.border_file = NULL;
    }
    display.changed = true;
}

char *rg_display_get_border(void)
{
    return rg_settings_get_string(NS_APP, SETTING_BORDER, NULL);
}

void rg_display_submit(const rg_surface_t *update, uint32_t flags)
{
    const int64_t time_start = rg_system_timer();

    // Those things should probably be asserted, but this is a new system let's be forgiving...
    if (!update || !update->data)
        return;

    if (display.source.width != update->width || display.source.height != update->height)
    {
        rg_display_sync(true);
        display.source.width = update->width;
        display.source.height = update->height;
        display.changed = true;
    }

    rg_task_send(display_task_queue, &(rg_task_msg_t){.dataPtr = update});

    counters.blockTime += rg_system_timer() - time_start;
    counters.totalFrames++;
}

bool rg_display_sync(bool block)
{
    while (block && rg_task_messages_waiting(display_task_queue))
        continue; // We should probably yield?
    return !rg_task_messages_waiting(display_task_queue);
}

void rg_display_write_rect(int left, int top, int width, int height, int stride, const uint16_t *buffer, uint32_t flags)
{
    RG_ASSERT_ARG(buffer);

    // calc stride before clipping width
    stride = RG_MAX(stride, width * 2);

    // Clipping
    width = RG_MIN(width, display.screen.width - left);
    height = RG_MIN(height, display.screen.height - top);

    // This can happen when left or top is out of bound
    if (width < 0 || height < 0)
        return;

    // This will work for now because we rarely draw from different threads (so all we need is ensure
    // that we're not interrupting a display update). But what we SHOULD be doing is acquire a lock
    // before every call to lcd_set_window and release it only after the last call to lcd_send_buffer.
    if (!(flags & RG_DISPLAY_WRITE_NOSYNC))
        rg_display_sync(true);

    // This isn't really necessary but it makes sense to invalidate
    // the lines we're about to overwrite...
    for (size_t y = 0; y < height; ++y)
        screen_line_checksum[top + y] = 0;

    lcd_set_window(left + display.screen.margins.left, top + display.screen.margins.top, width, height);

    for (size_t y = 0; y < height;)
    {
        uint16_t *lcd_buffer = lcd_get_buffer(LCD_BUFFER_LENGTH);
        size_t num_lines = RG_MIN(LCD_BUFFER_LENGTH / width, height - y);

        // Copy line by line because stride may not match width
        for (size_t line = 0; line < num_lines; ++line)
        {
            uint16_t *src = (void *)buffer + ((y + line) * stride);
            uint16_t *dst = lcd_buffer + (line * width);
            if (flags & RG_DISPLAY_WRITE_NOSWAP)
            {
                memcpy(dst, src, width * 2);
            }
            else
            {
                for (size_t i = 0; i < width; ++i)
                    dst[i] = (src[i] >> 8) | (src[i] << 8);
            }
        }

        lcd_send_buffer(lcd_buffer, width * num_lines);
        y += num_lines;
    }

    lcd_sync();
}

void rg_display_clear_rect(int left, int top, int width, int height, uint16_t color_le)
{
    const uint16_t color_be = (color_le << 8) | (color_le >> 8);
    int pixels_remaining = width * height;
    if (pixels_remaining > 0)
    {
        lcd_set_window(left + display.screen.margins.left, top + display.screen.margins.top, width, height);
        while (pixels_remaining > 0)
        {
            uint16_t *buffer = lcd_get_buffer(LCD_BUFFER_LENGTH);
            int pixels = RG_MIN(pixels_remaining, LCD_BUFFER_LENGTH);
            for (size_t j = 0; j < pixels; ++j)
                buffer[j] = color_be;
            lcd_send_buffer(buffer, pixels);
            pixels_remaining -= pixels;
        }
    }
}

void rg_display_clear_except(int left, int top, int width, int height, uint16_t color_le)
{
    // Clear everything except the specified area
    // FIXME: Do not ignore left/top...
    int left_offset = -display.screen.margins.left;
    int top_offset = -display.screen.margins.top;
    int horiz = (display.screen.real_width - width + 1) / 2;
    int vert = (display.screen.real_height - height + 1) / 2;
    rg_display_clear_rect(left_offset, top_offset, horiz, display.screen.real_height, color_le); // Left
    rg_display_clear_rect(left_offset + horiz + width, top_offset, horiz, display.screen.real_height, color_le); // Right
    rg_display_clear_rect(left_offset + horiz, top_offset, display.screen.real_width - horiz * 2, vert, color_le); // Top
    rg_display_clear_rect(left_offset + horiz, top_offset + vert + height, display.screen.real_width - horiz * 2, vert, color_le); // Bottom
}

void rg_display_clear(uint16_t color_le)
{
    // We ignore margins here, we want to fill the entire screen
    rg_display_clear_rect(-display.screen.margins.left, -display.screen.margins.top, display.screen.real_width,
                          display.screen.real_height, color_le);
}

void rg_display_deinit(void)
{
    rg_task_send(display_task_queue, &(rg_task_msg_t){.type = RG_TASK_MSG_STOP});
    // lcd_set_backlight(0);
    lcd_deinit();
    RG_LOGI("Display terminated.\n");
}

void rg_display_init(void)
{
    RG_LOGI("Initialization...\n");
    // TO DO: We probably should call the setters to ensure valid values...
    config = (rg_display_config_t){
        .backlight = rg_settings_get_number(NS_GLOBAL, SETTING_BACKLIGHT, 80),
        .scaling = rg_settings_get_number(NS_APP, SETTING_SCALING, RG_DISPLAY_SCALING_FIT),
        .filter = rg_settings_get_number(NS_APP, SETTING_FILTER, RG_DISPLAY_FILTER_BOTH),
        .rotation = rg_settings_get_number(NS_APP, SETTING_ROTATION, RG_DISPLAY_ROTATION_AUTO),
        .border_file = rg_settings_get_string(NS_APP, SETTING_BORDER, NULL),
        .custom_zoom = rg_settings_get_number(NS_APP, SETTING_CUSTOM_ZOOM, 1.0),
    };
    display = (rg_display_t){
        .screen.real_width = RG_SCREEN_WIDTH,
        .screen.real_height = RG_SCREEN_HEIGHT,
        .screen.width = RG_SCREEN_WIDTH,
        .screen.height = RG_SCREEN_HEIGHT,
        .screen.margins = RG_SCREEN_VISIBLE_AREA,
        .changed = true,
    };
    display.screen.width -= display.screen.margins.left + display.screen.margins.right;
    display.screen.height -= display.screen.margins.top + display.screen.margins.bottom;
    lcd_init();
    rg_display_clear(C_BLACK);
    rg_task_delay(80); // Wait for the screen be cleared before turning on the backlight (40ms doesn't seem to be enough...)
    lcd_set_backlight(config.backlight);
    display_task_queue = rg_task_create("rg_display", &display_task, NULL, 4 * 1024, RG_TASK_PRIORITY_6, 1);
    if (config.border_file)
        load_border_file(config.border_file);
    RG_LOGI("Display ready.\n");
}
