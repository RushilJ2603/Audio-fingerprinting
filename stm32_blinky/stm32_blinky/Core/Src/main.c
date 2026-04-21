/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : FFT Spectrum Analyzer — STM32F407 Discovery
  *
  * Blue button cycles through four synthesised test signals.  For each press
  * the board computes a Short-Time Fourier Transform (STFT) using the CMSIS-
  * DSP arm_rfft_fast_f32 routine and streams the per-frame magnitude spectrum
  * to the host over SWV / ITM port 0.  The companion fft_monitor.py script
  * reads those lines and renders a live frequency-spectrum bar chart.
  *
  * SWV protocol (one line per event, parsed by fft_monitor.py):
  *   BOOT                                  — board ready
  *   FFT_START sig=<n>                     — starting analysis on signal n
  *   FFT_FRAME <frame> <b0> … <b63>        — 64 uint8 magnitude bins
  *   FFT_DONE sig=<n>                      — all frames transmitted
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"

/* USER CODE BEGIN Includes */
#include "shazam.h"   /* fft_analyzer_init / fft_analyzer_run / fft_data_* */
/* USER CODE END Includes */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);

/* USER CODE BEGIN 0 */
#include <stdio.h>

/* Set by the EXTI0 ISR on every blue-button press. Polled & cleared in main. */
static volatile uint8_t g_button_pressed = 0;
/* USER CODE END 0 */

/**
  * @brief  Application entry point.
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

  HAL_Init();

  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  MX_GPIO_Init();

  /* USER CODE BEGIN 2 */

  /* Initialise FFT instance and pre-compute Hanning window. */
  fft_analyzer_init();

  /* Pre-synthesise signal 0 so the first button press feels instant. */
  fft_data_init();

  /* All LEDs off at startup. */
  HAL_GPIO_WritePin(GPIOD,
      LED_GREEN_Pin | LED_ORANGE_Pin | LED_RED_Pin | LED_BLUE_Pin,
      GPIO_PIN_RESET);

  printf("BOOT\n");

  uint8_t sig_idx = 0;   /* cycles 0 -> 1 -> 2 -> 3 -> 0 on each press */

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* ----------------------------------------------------------------
     * STATE 1 — WAIT
     * Pulse the blue LED (400 ms period, 10 ms poll) until the user
     * presses the blue button.
     * ---------------------------------------------------------------- */
    while (!g_button_pressed) {
      HAL_GPIO_TogglePin(LED_BLUE_GPIO_Port, LED_BLUE_Pin);
      for (int i = 0; i < 40 && !g_button_pressed; i++) {
        HAL_Delay(10);
      }
    }
    g_button_pressed = 0;
    HAL_GPIO_WritePin(LED_BLUE_GPIO_Port, LED_BLUE_Pin, GPIO_PIN_RESET);

    /* Advance the signal index on every press. */
    sig_idx = (sig_idx + 1) % NUM_TEST_SIGNALS;

    /* ----------------------------------------------------------------
     * STATE 2 — COMPUTE & STREAM
     * Announce the signal, run the STFT, stream FFT_FRAME lines.
     * The LED spinner inside fft_analyzer_run() shows progress.
     * ---------------------------------------------------------------- */
    printf("FFT_START sig=%u\n", (unsigned)sig_idx);

    fft_analyzer_run(sig_idx);

    printf("FFT_DONE sig=%u\n", (unsigned)sig_idx);

    /* ----------------------------------------------------------------
     * STATE 3 — RESULT FLASH
     * Brief LED burst so the user knows the run finished.
     *   sig 0 / 2 — green  (even-indexed signals)
     *   sig 1 / 3 — orange (odd-indexed signals)
     * ---------------------------------------------------------------- */
    uint16_t flash_pin = (sig_idx & 1u) ? LED_ORANGE_Pin : LED_GREEN_Pin;
    GPIO_TypeDef *flash_port = GPIOD;

    for (int i = 0; i < 3; i++) {
      HAL_GPIO_WritePin(flash_port, flash_pin, GPIO_PIN_SET);
      HAL_Delay(120);
      HAL_GPIO_WritePin(flash_port, flash_pin, GPIO_PIN_RESET);
      HAL_Delay(80);
    }

    /* Discard any button press that arrived during computation so we
     * return cleanly to the wait-pulse state. */
    g_button_pressed = 0;

    /* USER CODE END WHILE */
    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration — 16 MHz HSI, no PLL.
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                   | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
    Error_Handler();
}

/**
  * @brief GPIO Initialisation — LEDs (PD12-15) + user button (PA0, EXTI0).
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /* LEDs default low */
  HAL_GPIO_WritePin(GPIOD,
      LED_GREEN_Pin | LED_ORANGE_Pin | LED_RED_Pin | LED_BLUE_Pin,
      GPIO_PIN_RESET);

  /* PA0 — USER button, rising edge, pull-down */
  GPIO_InitStruct.Pin  = USER_BTN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(USER_BTN_GPIO_Port, &GPIO_InitStruct);

  /* PD12-15 — four LEDs, push-pull output */
  GPIO_InitStruct.Pin   = LED_GREEN_Pin | LED_ORANGE_Pin | LED_RED_Pin | LED_BLUE_Pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  HAL_NVIC_SetPriority(EXTI0_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);
}

/* USER CODE BEGIN 4 */

/* Button ISR — 50 ms debounce, flags main loop. */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
  static uint32_t last_tick = 0;
  if (pin != USER_BTN_Pin) return;
  uint32_t now = HAL_GetTick();
  if (now - last_tick < 50) return;
  last_tick = now;
  g_button_pressed = 1;
}

/* Route printf -> SWO/ITM port 0, read by pyocd's SWV stream. */
int __io_putchar(int ch)
{
  ITM_SendChar((uint32_t)(uint8_t)ch);
  return ch;
}

/* USER CODE END 4 */

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif
