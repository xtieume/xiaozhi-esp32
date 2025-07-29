#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include <wifi_station.h>
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>
#include "system_reset.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <esp_timer.h>
#include "i2c_device.h"
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "power_save_timer.h"
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <exception>
#include <stdexcept>
#include <sys/time.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_netif.h>

#define TAG "Spotpear_ESP32_S3_1_28_BOX"

LV_FONT_DECLARE(Noto);
LV_FONT_DECLARE(font_awesome_16_4);


class Cst816d : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };
    Cst816d(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        read_buffer_ = new uint8_t[6];
    }

    ~Cst816d() {
        delete[] read_buffer_;
    }

    void UpdateTouchPoint() {
        ReadRegs(0x02, read_buffer_, 6);
        if (read_buffer_[0] == 0xFF)
        {
            read_buffer_[0] = 0x00;
        }
        tp_.num = read_buffer_[0] & 0x01;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    const TouchPoint_t& GetTouchPoint() {
        return tp_;
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;
};


class CustomLcdDisplay : public SpiLcdDisplay {
private:
    lv_obj_t* info_container_ = nullptr;
    lv_obj_t* info_icon_label_ = nullptr;
    lv_obj_t* info_text_label_ = nullptr;
    const char* current_info_icon_ = nullptr;
    const char* current_info_text_ = nullptr;
    
    // Ticker variables for rotating information
    int info_rotation_index_ = 0;
    int64_t last_info_update_ = 0;
    const int64_t INFO_UPDATE_INTERVAL_MS = 3000; // 3 seconds per info

public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                    esp_lcd_panel_handle_t panel_handle,
                    int width,
                    int height,
                    int offset_x,
                    int offset_y,
                    bool mirror_x,
                    bool mirror_y,
                    bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy,
                    {
                        .text_font = &Noto,
                        .icon_font = &font_awesome_16_4,
                        .emoji_font = font_emoji_64_init(),
                    }) {

        DisplayLockGuard lock(this);
        // Tối ưu cho màn hình tròn với padding cho status bar
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.33, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.33, 0);
        
        // Tạo info ticker container
        info_container_ = lv_obj_create(content_);
        lv_obj_set_size(info_container_, LV_HOR_RES * 0.8, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(info_container_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(info_container_, 0, 0);
        lv_obj_set_style_pad_all(info_container_, 0, 0);
        lv_obj_align(info_container_, LV_ALIGN_TOP_MID, 0, LV_VER_RES * 0.42);
        
        // Tạo text label với scrolling
        info_text_label_ = lv_label_create(info_container_);
        lv_label_set_text(info_text_label_, "");
        lv_obj_set_style_text_font(info_text_label_, fonts_.text_font, 0);
        lv_obj_set_style_text_color(info_text_label_, current_theme_.text, 0);
        lv_obj_set_style_text_align(info_text_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(info_text_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(info_text_label_, LV_HOR_RES * 0.8);
        lv_obj_align(info_text_label_, LV_ALIGN_CENTER, 0, 0);
        
        // Điều chỉnh vị trí emoji và chat cho màn hình tròn
        if (emotion_label_ != nullptr) {
            lv_obj_align(emotion_label_, LV_ALIGN_TOP_MID, 0, LV_VER_RES * 0.25);
        }
        
        if (chat_message_label_ != nullptr) {
            lv_obj_align(chat_message_label_, LV_ALIGN_TOP_MID, 0, LV_VER_RES * 0.55);
            lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.8);
            lv_obj_set_height(chat_message_label_, LV_VER_RES * 0.35);
            lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
        }
    }
    
    // Method to get WiFi info
    std::string GetWiFiInfoString() {
        if (WifiStation::GetInstance().IsConnected()) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                char wifi_str[64];
                snprintf(wifi_str, sizeof(wifi_str), "%s (%ddBm)", 
                        (char*)ap_info.ssid, ap_info.rssi);
                return std::string(wifi_str);
            }
            return "WiFi Connected";
        }
        return "WiFi Disconnected";
    }
    
    // Method to get memory info
    std::string GetMemoryInfoString() {
        size_t free_heap = esp_get_free_heap_size();
        size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        int usage_percent = 100 - (free_heap * 100 / total_heap);
        
        char mem_str[32];
        snprintf(mem_str, sizeof(mem_str), "RAM: %d%% (%dKB free)", 
                usage_percent, (int)(free_heap / 1024));
        return std::string(mem_str);
    }
    
    // Method to get IP address
    std::string GetIpAddressString() {
        esp_netif_ip_info_t ip_info;
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ip_str[32];
            snprintf(ip_str, sizeof(ip_str), "IP: " IPSTR, IP2STR(&ip_info.ip));
            return std::string(ip_str);
        }
        return "IP: Not connected";
    }
    
    // Method to update rotating info display - gọi từ main loop
    void UpdateInfoTicker() {
        int64_t current_time = esp_timer_get_time() / 1000;
        
        // Check if it's time to rotate information
        if (current_time - last_info_update_ >= INFO_UPDATE_INTERVAL_MS) {
            last_info_update_ = current_time;
            
            std::string text;
            
            // Rotate through different information types (chỉ WiFi, Memory, IP)
            switch (info_rotation_index_) {
                case 0: // WiFi
                    text = GetWiFiInfoString();
                    break;
                case 1: // Memory
                    text = GetMemoryInfoString();
                    break;
                case 2: // IP Address
                    text = GetIpAddressString();
                    break;
                default:
                    info_rotation_index_ = -1; // Will be incremented to 0
                    return;
            }
            
            info_rotation_index_ = (info_rotation_index_ + 1) % 3; // Chỉ còn 3 loại
            
            // Update display (chỉ text, không có icon)
            if (info_text_label_ != nullptr) {
                DisplayLockGuard lock(this);
                current_info_text_ = text.c_str();
                lv_label_set_text(info_text_label_, text.c_str());
            }
        }
    }
};


class Spotpear_ESP32_S3_1_28_BOX : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    CustomLcdDisplay* display_;
    esp_timer_handle_t touchpad_timer_;
    esp_timer_handle_t info_ticker_timer_;
    Cst816d* cst816d_;
    PowerSaveTimer* power_save_timer_;
    esp_lcd_panel_handle_t panel_ = nullptr;

    void InitializePowerSaveTimer() {
        rtc_gpio_init(GPIO_NUM_3);
        rtc_gpio_set_direction(GPIO_NUM_3, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(GPIO_NUM_3, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, 31536000);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
            display_->SetChatMessage("system", "");
            display_->SetEmotion("sleepy");
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            display_->SetChatMessage("system", "");
            display_->SetEmotion("neutral");
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            rtc_gpio_set_level(GPIO_NUM_3, 0);
            // 启用保持功能，确保睡眠期间电平不变
            rtc_gpio_hold_en(GPIO_NUM_3);
            esp_lcd_panel_disp_on_off(panel_, false); //关闭显示
            esp_deep_sleep_start();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeCodecI2c_Touch() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = TP_PIN_NUM_TP_SDA,
            .scl_io_num = TP_PIN_NUM_TP_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }


    static void touchpad_timer_callback(void* arg) {
        auto& board = (Spotpear_ESP32_S3_1_28_BOX&)Board::GetInstance();
        auto touchpad = board.GetTouchpad();
        static bool was_touched = false;
        static int64_t touch_start_time = 0;
        const int64_t TOUCH_THRESHOLD_MS = 500;  // 触摸时长阈值，超过500ms视为长按

        touchpad->UpdateTouchPoint();
        auto touch_point = touchpad->GetTouchPoint();

        // 检测触摸开始
        if (touch_point.num > 0 && !was_touched) {
            was_touched = true;
            touch_start_time = esp_timer_get_time() / 1000; // 转换为毫秒
        }
        // 检测触摸释放
        else if (touch_point.num == 0 && was_touched) {
            was_touched = false;
            int64_t touch_duration = (esp_timer_get_time() / 1000) - touch_start_time;

            // 只有短触才触发
            if (touch_duration < TOUCH_THRESHOLD_MS) {
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting &&
                    !WifiStation::GetInstance().IsConnected()) {
                    board.ResetWifiConfiguration();
                }
                app.ToggleChatState();
            }
        }
    }

    static void info_ticker_timer_callback(void* arg) {
        auto& board = (Spotpear_ESP32_S3_1_28_BOX&)Board::GetInstance();
        if (board.display_ != nullptr) {
            board.display_->UpdateInfoTicker();
        }
    }

    void InitializeCst816DTouchPad() {
        ESP_LOGI(TAG, "Init Cst816D");
        cst816d_ = new Cst816d(i2c_bus_, 0x15);

        // 创建定时器，10ms 间隔
        esp_timer_create_args_t timer_args = {
            .callback = touchpad_timer_callback,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "touchpad_timer",
            .skip_unhandled_events = true,
        };

        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &touchpad_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(touchpad_timer_, 10 * 1000)); // 10ms = 10000us
    }

    void InitializeInfoTickerTimer() {
        // Tạo timer cho info ticker, 100ms interval để update smooth
        esp_timer_create_args_t timer_args = {
            .callback = info_ticker_timer_callback,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "info_ticker_timer",
            .skip_unhandled_events = true,
        };

        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &info_ticker_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(info_ticker_timer_, 100 * 1000)); // 100ms = 100000us
    }

    // SPI初始化
    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus");
        spi_bus_config_t buscfg = GC9A01_PANEL_BUS_SPI_CONFIG(DISPLAY_SPI_SCLK_PIN, DISPLAY_SPI_MOSI_PIN,
                                    DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // GC9A01初始化
    void InitializeGc9a01Display() {
        ESP_LOGI(TAG, "Init GC9A01 display");
        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_handle_t io_handle = NULL;
        esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(DISPLAY_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, 0, NULL);
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle));

        ESP_LOGI(TAG, "Install GC9A01 panel driver");
        esp_lcd_panel_handle_t panel_handle = NULL;
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_SPI_RESET_PIN;    // Set to -1 if not use
        panel_config.rgb_endian = LCD_RGB_ENDIAN_BGR;           //LCD_RGB_ENDIAN_RGB;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

        uint8_t data_0x62[] = { 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70 };
        esp_lcd_panel_io_tx_param(io_handle, 0x62, data_0x62, sizeof(data_0x62));

        uint8_t data_0x63[] = { 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70 };
        esp_lcd_panel_io_tx_param(io_handle, 0x63, data_0x63, sizeof(data_0x63));

        display_ = new CustomLcdDisplay(io_handle, panel_handle,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
    }

public:
    Spotpear_ESP32_S3_1_28_BOX() : boot_button_(BOOT_BUTTON_GPIO) {

        gpio_set_direction(TP_PIN_NUM_TP_INT, GPIO_MODE_INPUT);
        int level = gpio_get_level(TP_PIN_NUM_TP_INT);
        if (level == 1) {
            InitializeCodecI2c_Touch();
            InitializeCst816DTouchPad();
        }
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeSpi();
        InitializeGc9a01Display();
        InitializeInfoTickerTimer();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }


    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    Cst816d* GetTouchpad() {
        return cst816d_;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        if (!enabled) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveMode(enabled);
    }
};

DECLARE_BOARD(Spotpear_ESP32_S3_1_28_BOX);
