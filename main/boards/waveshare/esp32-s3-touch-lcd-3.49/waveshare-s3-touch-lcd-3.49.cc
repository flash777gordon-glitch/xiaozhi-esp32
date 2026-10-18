#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"

#include <esp_log.h>
#include "i2c_device.h"
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <esp_timer.h>
#include "esp_lcd_axs15231b.h"
#include "custom_lcd_display.h"
#include <lvgl.h>

#define TAG "waveshare_lcd_3_49"

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t []){0x00}, 0, 100},
    {0x29, (uint8_t []){0x00}, 0, 100},
};

class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    Button pwr_button_;
    LcdDisplay* display_;
    i2c_master_dev_handle_t disp_touch_dev_handle = NULL;
    lv_indev_t *touch_indev = NULL;

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize QSPI bus");
        spi_bus_config_t buscfg = {};
        buscfg.data0_io_num = LCD_D0;
        buscfg.data1_io_num = LCD_D1;
        buscfg.data2_io_num = LCD_D2;
        buscfg.data3_io_num = LCD_D3;
        buscfg.sclk_io_num = LCD_PCLK;
        buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // RESET PIN INIT
        gpio_config_t gpio_conf = {};
        gpio_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_conf.mode = GPIO_MODE_OUTPUT;
        gpio_conf.pin_bit_mask = ((uint64_t)0x01 << LCD_RST);
        gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
        
        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGI(TAG, "Install LCD driver");
        const axs15231b_vendor_config_t vendor_config = {
            .init_cmds = lcd_init_cmds,
            .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.vendor_config = (void *)&vendor_config;
        esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &panel);
        
        gpio_set_level(LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
        gpio_set_level(LCD_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(250));
        gpio_set_level(LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
        esp_lcd_panel_init(panel);

        display_ = new CustomLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeTouch() {
        i2c_master_bus_handle_t touch_i2c_bus_;
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = (i2c_port_t)I2C_NUM_1;
        i2c_bus_cfg.sda_io_num = I2C_Touch_SDA_PIN;
        i2c_bus_cfg.scl_io_num = I2C_Touch_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        if (i2c_new_master_bus(&i2c_bus_cfg, &touch_i2c_bus_) != ESP_OK) {
            ESP_LOGW(TAG, "Touch I2C bus init skipped");
            return;
        }
    
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = I2C_Touch_ADDRESS,
            .scl_speed_hz = 300000,
        };
        if (i2c_master_bus_add_device(touch_i2c_bus_, &dev_cfg, &disp_touch_dev_handle) == ESP_OK) {
            touch_indev = lv_indev_create();
            lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(touch_indev, TouchInputReadCallback);
            lv_indev_set_user_data(touch_indev, disp_touch_dev_handle);
        }
    }

    static void TouchInputReadCallback(lv_indev_t * indev, lv_indev_data_t *indevData) {
        i2c_master_dev_handle_t i2c_dev = (i2c_master_dev_handle_t)lv_indev_get_user_data(indev);
        uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};
        uint8_t buff[32] = {0};
        if (i2c_master_transmit_receive(i2c_dev, read_touchpad_cmd, 11, buff, 32, 100) == ESP_OK) {
            uint16_t pointX = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
            uint16_t pointY = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
            if (buff[1] > 0 && buff[1] < 5) {
                indevData->state = LV_INDEV_STATE_PRESSED;
                if (pointX > DISPLAY_WIDTH) pointX = DISPLAY_WIDTH;
                if (pointY > DISPLAY_HEIGHT) pointY = DISPLAY_HEIGHT;
                indevData->point.x = pointY;
                indevData->point.y = (DISPLAY_HEIGHT - pointX);
                return;
            }
        }
        indevData->state = LV_INDEV_STATE_RELEASED;
    }

    void EnableAmplifier() {
        if (AUDIO_CODEC_PA_PIN != GPIO_NUM_NC) {
            gpio_config_t io_conf = {};
            io_conf.mode = GPIO_MODE_OUTPUT;
            io_conf.pin_bit_mask = (1ULL << AUDIO_CODEC_PA_PIN);
            gpio_config(&io_conf);
            gpio_set_level(AUDIO_CODEC_PA_PIN, 1);
        }
    }

public:
    CustomBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        pwr_button_(PWR_BUTTON_GPIO) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeTouch();
        EnableAmplifier();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecDuplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(CustomBoard);
