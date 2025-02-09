// BlueCubeMod Firmware
// Created by Nathan Reeves 2019

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/periph_ctrl.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "esp_hidd_api.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "soc/rmt_reg.h"

#define LED_GPIO 12
#define PIN_SEL (1ULL << LED_GPIO)

#define PRO_CON 0x03
// #define JOYCON_L 0x01
// #define JOYCON_R 0x02

#define CONTROLLER_TYPE PRO_CON

typedef struct 
{
    // Buttons
    uint8_t A;
    uint8_t B;
    uint8_t X;
    uint8_t Y;

    // Triggers
    uint8_t L;
    uint8_t R;
    uint8_t ZL;
    uint8_t ZR;

    // Dpad
    uint8_t Dpad_Up;
    uint8_t Dpad_Down;
    uint8_t Dpad_Left;
    uint8_t Dpad_Right;

    // Functions
    uint8_t Plus;
    uint8_t Minus;
    uint8_t Capture;
    uint8_t Home;

    // Sticks
    uint8_t StickL_X;
    uint8_t StickL_Y;
    uint8_t StickL_Click;
    uint8_t StickR_X;
    uint8_t StickR_Y;
    uint8_t StickR_Click;

} ControlInputStatus;

// Dpad input defines
#define A_DPAD_CENTER 0x08
#define A_DPAD_U 0x00
#define A_DPAD_U_R 0x01
#define A_DPAD_R 0x02
#define A_DPAD_D_R 0x03
#define A_DPAD_D 0x04
#define A_DPAD_D_L 0x05
#define A_DPAD_L 0x06
#define A_DPAD_U_L 0x07

// From least to most significant bits:
static uint8_t but1_send = 0; // (Right) Y, X, B, A, SR, SL, R, ZR
static uint8_t but2_send = 0; // (Shared) -, +, Rs, Ls, H, Cap, --, Charging Grip
static uint8_t but3_send = 0; // (Left) D, U, R, L, SR, SL, L, ZL

static uint8_t lx_send = 128;
static uint8_t ly_send = 128;
static uint8_t rx_send = 128;
static uint8_t ry_send = 128;

bool connected = false;
bool paired = false;

SemaphoreHandle_t xSemaphore;

TaskHandle_t ButtonsHandle = NULL;
TaskHandle_t SendingHandle = NULL;
TaskHandle_t BlinkHandle = NULL;

static esp_hidd_app_param_t app_param;
static esp_hidd_qos_param_t both_qos;

// Timer has +1 added to it every send cycle
// Apparently, it can be used to detect packet loss/excess latency
static uint8_t timer = 0;

static uint8_t report30[48] = {[0] = 0x00, [1] = 0x8E, [11] = 0x80};
static uint8_t dummy[11] = {0x00, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80, 0x00, 0x08, 0x80};

#define UART_NUM (UART_NUM_0)
#define UART_TXD_PIN (UART_PIN_NO_CHANGE) // When UART2, TX GPIO_NUM_19, RX GPIO_NUM_26
#define UART_RXD_PIN (UART_PIN_NO_CHANGE) // When UART0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE

uart_config_t uart_config;
QueueHandle_t uart_queue;
#define BUF_SIZE (256)
uint8_t* uart_data;

void uart_init()
{
  uart_config.baud_rate = 9600;
  uart_config.data_bits = UART_DATA_8_BITS;
  uart_config.parity = UART_PARITY_DISABLE;
  uart_config.stop_bits = UART_STOP_BITS_1;
  uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

  uart_param_config(UART_NUM, &uart_config);
  uart_set_pin(UART_NUM, UART_TXD_PIN, UART_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 10, &uart_queue, 0));

  uart_data = (uint8_t*)malloc(BUF_SIZE);
}

static void uart_task()
{
  ESP_LOGI("uart", "Recieving uart packets on core %d\n", xPortGetCoreID());

  uint8_t recieved_uart_data_count = 0;
  uint8_t recieved_uart_data[11];
  memset(recieved_uart_data, 0, 11);

  while (1)
  {
    int len = uart_read_bytes(UART_NUM, uart_data, BUF_SIZE, portTICK_RATE_MS);

    // 受信データがある
    if (len > 0)
    {
      for(int i=0;i<len;i++)
      {
        recieved_uart_data[recieved_uart_data_count++] = uart_data[i];

        // 1フレーム分11バイト受信した
        if(recieved_uart_data_count == 11)
        {
          recieved_uart_data_count = 0;

          ESP_LOGI(
            "uart",
            "Packet data: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            recieved_uart_data[0], recieved_uart_data[1], recieved_uart_data[2], recieved_uart_data[3],
            recieved_uart_data[4], recieved_uart_data[5], recieved_uart_data[6], recieved_uart_data[7],
            recieved_uart_data[8], recieved_uart_data[9], recieved_uart_data[10]);

          // 受信したデータの正当性を確認する
          if((recieved_uart_data[0] != 0xAA) || (recieved_uart_data[1] != 0xAA) ||
             (recieved_uart_data[2] != 0xAA) || (recieved_uart_data[3] != 0xAA) ||
             (recieved_uart_data[4] != 0xAA) || (recieved_uart_data[10] != 0x00))
          {
            // 受信したデータの形が不正だった
            uart_flush(UART_NUM);
            memset(recieved_uart_data, 0, 11);
          }
          else // 受信したデータの形が正常だった
          {
            // 入力情報をまとめる
            ControlInputStatus inputStatus;

            inputStatus.ZR = ((recieved_uart_data[5] >> 7) & 1); // ZR
            inputStatus.ZL = ((recieved_uart_data[5] >> 6) & 1); // ZL
            inputStatus.R = ((recieved_uart_data[5] >> 5) & 1); // R
            inputStatus.L = ((recieved_uart_data[5] >> 4) & 1); // L
            inputStatus.X = ((recieved_uart_data[5] >> 3) & 1); // X
            inputStatus.A = ((recieved_uart_data[5] >> 2) & 1); // A
            inputStatus.B = ((recieved_uart_data[5] >> 1) & 1); // B
            inputStatus.Y = (recieved_uart_data[5] & 1); // Y

            inputStatus.Capture = ((recieved_uart_data[6] >> 5) & 1); // Capture
            inputStatus.Home = ((recieved_uart_data[6] >> 4) & 1); // Home
            inputStatus.StickR_Click = ((recieved_uart_data[6] >> 3) & 1); // StickR_Click
            inputStatus.StickL_Click = ((recieved_uart_data[6] >> 2) & 1); // StickL_Click            
            inputStatus.Plus = ((recieved_uart_data[6] >> 1) & 1); // Plus
            inputStatus.Minus = (recieved_uart_data[6] & 1); // Minus

            // Dpad initialize
            inputStatus.Dpad_Up = 0;
            inputStatus.Dpad_Down = 0;
            inputStatus.Dpad_Left = 0;
            inputStatus.Dpad_Right = 0;

            switch(recieved_uart_data[7])
            {
              case A_DPAD_U:
                inputStatus.Dpad_Up = 1;
                break;
              case A_DPAD_R:
                inputStatus.Dpad_Right = 1;
                break;
              case A_DPAD_D:
                inputStatus.Dpad_Down = 1;
                break;
              case A_DPAD_L:
                inputStatus.Dpad_Left = 1;
                break;
              case A_DPAD_U_R:
                inputStatus.Dpad_Up = 1;
                inputStatus.Dpad_Right = 1;
                break;
              case A_DPAD_U_L:
                inputStatus.Dpad_Up = 1;
                inputStatus.Dpad_Left = 1;
                break;
              case A_DPAD_D_R:
                inputStatus.Dpad_Down = 1;
                inputStatus.Dpad_Right = 1;
                break;
              case A_DPAD_D_L:
                inputStatus.Dpad_Down = 1;
                inputStatus.Dpad_Left = 1;
                break;
              case A_DPAD_CENTER:
              default:
                break;
            }

            // Stick initialize
            inputStatus.StickL_X = 128;
            inputStatus.StickL_Y = 128;
            inputStatus.StickR_X = 128;
            inputStatus.StickR_Y = 128;

            // StickL_X
            if(recieved_uart_data[8] & 0x01) // Left
            {
              inputStatus.StickL_X = 0;
            }
            else if(recieved_uart_data[8]  & 0x02) // Right
            {
              inputStatus.StickL_X = 255;
            }

            // StickL_Y
            if(recieved_uart_data[8] & 0x04) // Up
            {
              inputStatus.StickL_Y = 255;
            }
            else if(recieved_uart_data[8] & 0x08) // Down
            {
              inputStatus.StickL_Y = 0;
            }

            // StickR_X
            if(recieved_uart_data[9] & 0x01) // Left
            {
              inputStatus.StickR_X = 0;
            }
            else if(recieved_uart_data[9]  & 0x02) // Right
            {
              inputStatus.StickR_X = 255;
            }

            // StickR_Y
            if(recieved_uart_data[9] & 0x04) // Up
            {
              inputStatus.StickR_Y = 255;
            }
            else if(recieved_uart_data[9] & 0x08) // Down
            {
              inputStatus.StickR_Y = 0;
            }

            // まとめた入力情報を送信用データにセットする
            but1_send = (inputStatus.Y) +       // Y
                        (inputStatus.X << 1) +  // X
                        (inputStatus.B << 2) +  // B
                        (inputStatus.A << 3) +  // A
                        (inputStatus.R << 6) +  // R
                        (inputStatus.ZR << 7);  // ZR

            but2_send = (inputStatus.Minus) +     // Minus/Select
                        (inputStatus.Plus << 1) + // Plus/Start
                        (inputStatus.StickR_Click << 2) + // R Stick Click
                        (inputStatus.StickL_Click << 3) + // L Stick Click
                        (inputStatus.Home << 4) +   // Home
                        (inputStatus.Capture << 5); // Capture      

            but3_send = (inputStatus.Dpad_Down) +       // Dpad_Down
                        (inputStatus.Dpad_Up << 1) +    // Dpad_Up
                        (inputStatus.Dpad_Right << 2) + // Dpad_Right
                        (inputStatus.Dpad_Left << 3) +  // Dpad_Left
                        (inputStatus.L << 6) + // L
                        (inputStatus.ZL << 7); // ZL

            lx_send = inputStatus.StickL_X;
            ly_send = inputStatus.StickL_Y;
            rx_send = inputStatus.StickR_X;
            ry_send = inputStatus.StickR_Y;
          }
        }
      }

      if (but1_send || but2_send || but3_send)
      {
        ESP_LOGI("uart", "but1: %d, but2: %d, but3: %d\n", but1_send, but2_send, but3_send);
      }

      if ((lx_send != 128) || (ly_send != 128) ||
          (rx_send != 128) || (ry_send != 128))
      {
        ESP_LOGI("uart", "lx: %d, ly: %d, cx: %d, cy: %d\n", lx_send, ly_send, rx_send, ry_send);
      }
    }
  }
  vTaskDelete(NULL);
}

void send_buttons()
{
  xSemaphoreTake(xSemaphore, portMAX_DELAY);
  report30[0] = timer;
  dummy[0] = timer;
  // buttons
  report30[2] = but1_send;
  report30[3] = but2_send;
  report30[4] = but3_send;
  // encode left stick
  report30[5] = (lx_send << 4) & 0xF0;
  report30[6] = (lx_send & 0xF0) >> 4;
  report30[7] = ly_send;
  // encode right stick
  report30[8] = (rx_send << 4) & 0xF0;
  report30[9] = (rx_send & 0xF0) >> 4;
  report30[10] = ry_send;
  xSemaphoreGive(xSemaphore);

  if(timer == 255)
  {
    timer = 0;
  }
  else
  {
    timer += 1;
  }

  if (paired || connected)
  {
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x30, sizeof(report30), report30);
    vTaskDelay(15);
  }
  else
  {
    esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x30, sizeof(dummy), dummy);
    vTaskDelay(100);
  }
}

/// Switch Replies

// Reply for REQUEST_DEVICE_INFO
static uint8_t reply02[] = {
  0x00, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x82, 0x02, 0x04, 0x00,
  CONTROLLER_TYPE,
        0x02, 0xD4, 0xF0, 0x57, 0x6E, 0xF0, 0xD7,
  0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Reply for SET_SHIPMENT_STATE
static uint8_t reply08[] = {
  0x01, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x80, 0x08, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Reply for SET_INPUT_REPORT_MODE
static uint8_t reply03[] = {
  0x04, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x80, 0x03, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Trigger buttons elapsed time
static uint8_t reply04[] = {
  0x0A, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x83, 0x04, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x01,
  0x2c, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Serial number and controller type (although, our code doesn't read (and as
// such, report) the controller type from here.)
static uint8_t spi_reply_address_0[] = {
  0x02, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x90, 0x10, 0x00, 0x60,
  0x00, 0x00, 0x10, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0x00, 0x00, CONTROLLER_TYPE, 0xA0, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0
};
// The CONTROLLER_TYPE is technically unused, but it makes me feel better.

// SPI Flash colors
static uint8_t spi_reply_address_0x50[] = {
  0x03, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00,
  0x00, 0x00, 0x90, 0x10, 0x50, 0x60, 0x00, 0x00, 0x0D, // Start of colors
  0x23, 0x23, 0x23,                                     // Body color
  0xff, 0xff, 0xff,                                     // Buttons color
  0x95, 0x15, 0x15, // Left Grip color (Pro Con)
  0x15, 0x15, 0x95, // Right Grip color (Pro Con)
  0xff,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t spi_reply_address_0x80[] = {
  0x0B, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x90, 0x10, 0x80, 0x60,
  0x00, 0x00, 0x18, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t spi_reply_address_0x98[] = {
  0x0C, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x90, 0x10, 0x98, 0x60,
  0x00, 0x00, 0x12, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// User analog stick calib
static uint8_t spi_reply_address_0x10[] = {
  0x0D, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x90, 0x10, 0x10, 0x80,
  0x00, 0x00, 0x18, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t spi_reply_address_0x3d[] = {
  0x0E, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x90, 0x10, 0x3D, 0x60,
  0x00, 0x00, 0x19, 0x00, 0x07, 0x70, 0x00, 0x08,
  0x80, 0x00, 0x07, 0x70, 0x00, 0x08, 0x80, 0x00,
  0x07, 0x70, 0x00, 0x07, 0x70, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00
};

static uint8_t spi_reply_address_0x20[] = {
  0x10, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x90, 0x10, 0x20, 0x60,
  0x00, 0x00, 0x18, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Reply for changing the status of the IMU IMU (6-Axis sensor)
static uint8_t reply4001[] = {
  0x15, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x80, 0x40, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t reply4801[] = {
  0x1A, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x80, 0x48, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Reply for SubCommand.SET_PLAYER_LIGHTS
static uint8_t reply3001[] = {
  0x1C, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x80, 0x30, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t reply3333[] = {
  0x31, 0x8e, 0x00, 0x00, 0x00, 0x00, 0x08, 0x80,
  0x00, 0x08, 0x80, 0x00, 0xa0, 0x21, 0x01, 0x00,
  0x00, 0x00, 0x03, 0x00, 0x05, 0x01, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7b,
  0x00
};

// Reply for SubCommand.SET_NFC_IR_MCU_STATE
static uint8_t reply3401[] = {
  0x12, 0x8e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x08, 0x80, 0x00, 0x80, 0x22, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t hid_descriptor[] = {
  0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x06, 0x01,
  0xff, 0x85, 0x21, 0x09, 0x21, 0x75, 0x08, 0x95,
  0x30, 0x81, 0x02, 0x85, 0x30, 0x09, 0x30, 0x75,
  0x08, 0x95, 0x30, 0x81, 0x02, 0x85, 0x31, 0x09,
  0x31, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x02,
  0x85, 0x32, 0x09, 0x32, 0x75, 0x08, 0x96, 0x69,
  0x01, 0x81, 0x02, 0x85, 0x33, 0x09, 0x33, 0x75,
  0x08, 0x96, 0x69, 0x01, 0x81, 0x02, 0x85, 0x3f,
  0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00,
  0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
  0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
  0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x05, 0x09,
  0x75, 0x04, 0x95, 0x01, 0x81, 0x01, 0x05, 0x01,
  0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34,
  0x16, 0x00, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00,
  0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0x06, 0x01,
  0xff, 0x85, 0x01, 0x09, 0x01, 0x75, 0x08, 0x95,
  0x30, 0x91, 0x02, 0x85, 0x10, 0x09, 0x10, 0x75,
  0x08, 0x95, 0x30, 0x91, 0x02, 0x85, 0x11, 0x09,
  0x11, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02, 0x85,
  0x12, 0x09, 0x12, 0x75, 0x08, 0x95, 0x30, 0x91,
  0x02, 0xc0
};

int hid_descriptor_len = sizeof(hid_descriptor);

// sending bluetooth values every 15ms
void send_task(void* pvParameters)
{
  const char* TAG = "send_task";
  ESP_LOGI(TAG, "Sending hid reports on core %d\n", xPortGetCoreID());

  while(1)
  {
    send_buttons();
  }

  vTaskDelete(NULL);
}

// LED blink
void startBlink()
{
  while(1)
  {
    gpio_set_level(LED_GPIO, 0);
    vTaskDelay(150);
    gpio_set_level(LED_GPIO, 1);
    vTaskDelay(150);
    gpio_set_level(LED_GPIO, 0);
    vTaskDelay(150);
    gpio_set_level(LED_GPIO, 1);
    vTaskDelay(1000);
  }

  vTaskDelete(NULL);
}

/*
void set_bt_address()
{
  // store a random MAC address in flash
  nvs_handle my_handle;
  esp_err_t err;
  uint8_t bt_addr[8];

  err = nvs_open("storage", NVS_READWRITE, &my_handle);
  if (err != ESP_OK) return err;

  size_t addr_size = 0;
  err = nvs_get_blob(my_handle, "mac_addr", NULL, &addr_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;

  if (addr_size > 0)
  {
    err = nvs_get_blob(my_handle, "mac_addr", bt_addr, &addr_size);
  }
  else
  {
    for(int i=0; i<8; i++)
    {
      bt_addr[i] = esp_random()%255;
    }
    size_t addr_size = sizeof(bt_addr);
    err = nvs_set_blob(my_handle, "mac_addr", bt_addr, addr_size);
  }

  err = nvs_commit(my_handle);
  nvs_close(my_handle);
  esp_base_mac_addr_set(bt_addr);

  // put mac addr in switch pairing packet
  for(int z=0;z<6;z++)
  {
    reply02[z+19] = bt_addr[z];
  }
}

void print_bt_address()
{
  const char* TAG = "bt_address";
  const uint8_t* bd_addr;

  bd_addr = esp_bt_dev_get_address();
  ESP_LOGI(TAG, "bluetooth address is %02X:%02X:%02X:%02X:%02X:%02X",
    bd_addr[0], bd_addr[1], bd_addr[2], bd_addr[3], bd_addr[4], bd_addr[5]);
}
*/

static void esp_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
  static const char* TAG = "esp_bt_gap_cb";
  switch(event)
  {
  case ESP_BT_GAP_DISC_RES_EVT:
    ESP_LOGI(TAG, "ESP_BT_GAP_DISC_RES_EVT");
    esp_log_buffer_hex(TAG, param->disc_res.bda, ESP_BD_ADDR_LEN);
    break;
  case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
    ESP_LOGI(TAG, "ESP_BT_GAP_DISC_STATE_CHANGED_EVT");
    break;
  case ESP_BT_GAP_RMT_SRVCS_EVT:
    ESP_LOGI(TAG, "ESP_BT_GAP_RMT_SRVCS_EVT");
    ESP_LOGI(TAG, "%d", param->rmt_srvcs.num_uuids);
    break;
  case ESP_BT_GAP_RMT_SRVC_REC_EVT:
    ESP_LOGI(TAG, "ESP_BT_GAP_RMT_SRVC_REC_EVT");
    break;
  case ESP_BT_GAP_AUTH_CMPL_EVT:
    if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
    {
      ESP_LOGI(TAG, "authentication success: %s", param->auth_cmpl.device_name);
      esp_log_buffer_hex(TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
    }
    else
    {
      ESP_LOGE(TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
    }
    break;
  default:
    break;
  }
}

void esp_bt_hidd_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
  static const char* TAG = "esp_bt_hidd_cb";
  switch (event)
  {
  case ESP_HIDD_INIT_EVT:
    if (param->init.status == ESP_HIDD_SUCCESS)
    {
      ESP_LOGI(TAG, "setting hid parameters");
      esp_bt_hid_device_register_app(&app_param, &both_qos, &both_qos);
    }
    else
    {
      ESP_LOGE(TAG, "init hidd failed!");
    }
    break;
  case ESP_HIDD_DEINIT_EVT:
    break;
  case ESP_HIDD_REGISTER_APP_EVT:
    if (param->register_app.status == ESP_HIDD_SUCCESS)
    {
      ESP_LOGI(TAG, "setting hid parameters success!");
      ESP_LOGI(TAG, "setting to connectable, discoverable");
      esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
      if (param->register_app.in_use && param->register_app.bd_addr != NULL)
      {
        ESP_LOGI(TAG, "start virtual cable plug!");
        esp_bt_hid_device_connect(param->register_app.bd_addr);
      }
    }
    else
    {
      ESP_LOGE(TAG, "setting hid parameters failed!");
    }
    break;
  case ESP_HIDD_UNREGISTER_APP_EVT:
    if (param->unregister_app.status == ESP_HIDD_SUCCESS)
    {
      ESP_LOGI(TAG, "unregister app success!");
    }
    else
    {
      ESP_LOGE(TAG, "unregister app failed!");
    }
    break;
  case ESP_HIDD_OPEN_EVT:
    if (param->open.status == ESP_HIDD_SUCCESS)
    {
      if (param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTING)
      {
        ESP_LOGI(TAG, "connecting...");
      }
      else if (param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTED)
      {
        ESP_LOGI(TAG, "connected to %02x:%02x:%02x:%02x:%02x:%02x", param->open.bd_addr[0],
          param->open.bd_addr[1], param->open.bd_addr[2], param->open.bd_addr[3], param->open.bd_addr[4],
          param->open.bd_addr[5]);
        ESP_LOGI(TAG, "making self non-discoverable and non-connectable.");
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

        xSemaphoreTake(xSemaphore, portMAX_DELAY);
        connected = true;
        xSemaphoreGive(xSemaphore);

        //restart send_task
        if(SendingHandle != NULL)
        {
          vTaskDelete(SendingHandle);
          SendingHandle = NULL;
        }
        xTaskCreatePinnedToCore(send_task, "send_task", 4096, NULL, 2, &SendingHandle, 0);
      }
      else
      {
        ESP_LOGE(TAG, "unknown connection status");
      }
    }
    else
    {
      ESP_LOGE(TAG, "open failed!");
    }
    break;
  case ESP_HIDD_CLOSE_EVT:
    ESP_LOGI(TAG, "ESP_HIDD_CLOSE_EVT");
    if (param->close.status == ESP_HIDD_SUCCESS)
    {
      if (param->close.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTING)
      {
        ESP_LOGI(TAG, "disconnecting...");
      }
      else if (param->close.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED)
      {
        ESP_LOGI(TAG, "disconnected!");
        ESP_LOGI(TAG, "making self discoverable and connectable again.");
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

        xSemaphoreTake(xSemaphore, portMAX_DELAY);
        connected = false;
        xSemaphoreGive(xSemaphore);
      }
      else
      {
        ESP_LOGE(TAG, "unknown connection status");
      }
    }
    else
    {
      ESP_LOGE(TAG, "close failed!");
    }
    break;
  case ESP_HIDD_SEND_REPORT_EVT:
    ESP_LOGI(TAG, "ESP_HIDD_SEND_REPORT_EVT id:0x%02x, type:%d", param->send_report.report_id,
      param->send_report.report_type);
    break;
  case ESP_HIDD_REPORT_ERR_EVT:
    ESP_LOGI(TAG, "ESP_HIDD_REPORT_ERR_EVT");
    break;
  case ESP_HIDD_GET_REPORT_EVT:
    ESP_LOGI(TAG, "ESP_HIDD_GET_REPORT_EVT id:0x%02x, type:%d, size:%d", param->get_report.report_id,
      param->get_report.report_type, param->get_report.buffer_size);
    break;
  case ESP_HIDD_SET_REPORT_EVT:
    ESP_LOGI(TAG, "ESP_HIDD_SET_REPORT_EVT");
    break;
  case ESP_HIDD_SET_PROTOCOL_EVT:
    ESP_LOGI(TAG, "ESP_HIDD_SET_PROTOCOL_EVT");
    if (param->set_protocol.protocol_mode == ESP_HIDD_BOOT_MODE)
    {
      ESP_LOGI(TAG, "  - boot protocol");
    }
    else if (param->set_protocol.protocol_mode == ESP_HIDD_REPORT_MODE)
    {
      ESP_LOGI(TAG, "  - report protocol");
    }
    break;
  case ESP_HIDD_INTR_DATA_EVT:
    ESP_LOGI(TAG, "ESP_HIDD_INTR_DATA_EVT id:0x%02x", param->intr_data.report_id);
    esp_log_buffer_hex(TAG, param->intr_data.data, param->intr_data.len);
    if (param->intr_data.data[9] == 2)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply02), reply02);
      ESP_LOGI(TAG, "reply02");
    }
    if (param->intr_data.data[9] == 8)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply08), reply08);
      ESP_LOGI(TAG, "reply08");
    }
    if (param->intr_data.data[9] == 16 && param->intr_data.data[10] == 0 && param->intr_data.data[11] == 96)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0), spi_reply_address_0);
      ESP_LOGI(TAG, "replyspi0");
    }
    if (param->intr_data.data[9] == 16 && param->intr_data.data[10] == 80 && param->intr_data.data[11] == 96) {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0x50), spi_reply_address_0x50);
      ESP_LOGI(TAG, "replyspi50");
    }
    if (param->intr_data.data[9] == 3)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply03), reply03);
      ESP_LOGI(TAG, "reply03");
    }
    if (param->intr_data.data[9] == 4)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply04), reply04);
      ESP_LOGI(TAG, "reply04");
    }
    if (param->intr_data.data[9] == 16 && param->intr_data.data[10] == 128 && param->intr_data.data[11] == 96)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0x80), spi_reply_address_0x80);
      ESP_LOGI(TAG, "replyspi80");
    }
    if (param->intr_data.data[9] == 16 && param->intr_data.data[10] == 152 && param->intr_data.data[11] == 96)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0x98), spi_reply_address_0x98);
      ESP_LOGI(TAG, "replyspi98");
    }
    if (param->intr_data.data[9] == 16 && param->intr_data.data[10] == 16 && param->intr_data.data[11] == 128)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0x10), spi_reply_address_0x10);
      ESP_LOGI(TAG, "replyspi10");
    }
    if (param->intr_data.data[9] == 16 && param->intr_data.data[10] == 61 && param->intr_data.data[11] == 96)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0x3d), spi_reply_address_0x3d);
      ESP_LOGI(TAG, "reply3d");
    }
    if (param->intr_data.data[9] == 16 && param->intr_data.data[10] == 32 && param->intr_data.data[11] == 96)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(spi_reply_address_0x20), spi_reply_address_0x20);
      ESP_LOGI(TAG, "replyspi20");
    }
    if (param->intr_data.data[9] == 64)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply4001), reply4001);
      ESP_LOGI(TAG, "reply4001");
    }
    if (param->intr_data.data[9] == 72)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply4801), reply4801);
      ESP_LOGI(TAG, "reply4801");
    }
    if (param->intr_data.data[9] == 34)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply3401), reply3401);
      ESP_LOGI(TAG, "reply3401");
    }
    if (param->intr_data.data[9] == 48)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply3001), reply3001);
      ESP_LOGI(TAG, "reply3001");
      // if (CONTROLLER_TYPE == JOYCON_L)
      // {
      //   paired = true;
      // }
    }
    if (param->intr_data.data[9] == 33 && param->intr_data.data[10] == 33)
    {
      esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0x21, sizeof(reply3333), reply3333);
      ESP_LOGI(TAG, "reply3333");
      paired = true;
    }
    break;
  case ESP_HIDD_VC_UNPLUG_EVT:
    ESP_LOGI(TAG, "ESP_HIDD_VC_UNPLUG_EVT");
    if (param->vc_unplug.status == ESP_HIDD_SUCCESS)
    {
      if (param->close.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED)
      {
        ESP_LOGI(TAG, "disconnected!");

        ESP_LOGI(TAG, "making self discoverable and connectable again.");
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
      }
      else
      {
        ESP_LOGE(TAG, "unknown connection status");
      }
    }
    else
    {
      ESP_LOGE(TAG, "close failed!");
    }
    break;
  default:
      break;
  }
}

void app_main()
{
  esp_log_level_set("*", ESP_LOG_ERROR);
  // esp_log_level_set("*", ESP_LOG_WARN);
  // esp_log_level_set("*", ESP_LOG_INFO);

  // esp_log_level_set("uart", ESP_LOG_INFO);

  uart_init();
  xTaskCreatePinnedToCore(uart_task, "uart_task", 2048, NULL, 1, &ButtonsHandle, 1);

  // flash LED
  vTaskDelay(100);
  gpio_set_level(LED_GPIO, 0);
  vTaskDelay(100);
  gpio_set_level(LED_GPIO, 1);
  vTaskDelay(100);
  gpio_set_level(LED_GPIO, 0);
  vTaskDelay(100);
  gpio_set_level(LED_GPIO, 1);
  vTaskDelay(100);
  gpio_set_level(LED_GPIO, 0);

  const char* TAG = "app_main";
  esp_err_t ret;
  static esp_bt_cod_t dclass;

  xSemaphore = xSemaphoreCreateMutex();

  gpio_config_t io_conf;
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = PIN_SEL;
  io_conf.pull_down_en = 0;
  io_conf.pull_up_en = 0;
  gpio_config(&io_conf);

  // 一応名前とプロバイダーを純正と一緒にする ( For now, set these the same as a genuine product )
  app_param.name = "Wireless Gamepad";
  app_param.description = "Gamepad";
  app_param.provider = "Nintendo";
  app_param.subclass = 0x8;
  app_param.desc_list = hid_descriptor;
  app_param.desc_list_len = hid_descriptor_len;
  memset(&both_qos, 0, sizeof(esp_hidd_qos_param_t));

  dclass.minor = 2;
  dclass.major = 5;
  dclass.service = 1;

	ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK( ret );

	ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK)
  {
    ESP_LOGE(TAG, "initialize controller failed: %s\n",  esp_err_to_name(ret));
    return;
  }

  if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK)
  {
    ESP_LOGE(TAG, "enable controller failed: %s\n",  esp_err_to_name(ret));
    return;
  }

  if ((ret = esp_bluedroid_init()) != ESP_OK)
  {
    ESP_LOGE(TAG, "initialize bluedroid failed: %s\n",  esp_err_to_name(ret));
    return;
  }

  if ((ret = esp_bluedroid_enable()) != ESP_OK)
  {
    ESP_LOGE(TAG, "enable bluedroid failed: %s\n",  esp_err_to_name(ret));
    return;
  }

  if ((ret = esp_bt_gap_register_callback(esp_bt_gap_cb)) != ESP_OK)
  {
    ESP_LOGE(TAG, "gap register failed: %s\n", esp_err_to_name(ret));
    return;
  }

  ESP_LOGI(TAG, "setting device name");
  esp_bt_dev_set_device_name("Pro Controller");

  ESP_LOGI(TAG, "setting hid device class");
  esp_bt_gap_set_cod(dclass, ESP_BT_SET_COD_ALL);

  ESP_LOGI(TAG, "setting hid parameters");
  esp_bt_hid_device_register_callback(esp_bt_hidd_cb);

  ESP_LOGI(TAG, "starting hid device");
  esp_bt_hid_device_init();

  // esp_hid_device_connect

  // start blinking
  xTaskCreate(startBlink, "blink_task", 1024, NULL, 2, &BlinkHandle);
}
