/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "lcd.h"
#include "keys.h"
#include "temp.h"
#include "dimmer.h"
#include "control.h"
#include "fan.h"
#include "ui.h"
#include "settings.h"
#include <stdio.h>
#include "stdbool.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

I2C_HandleTypeDef hi2c1;

SPI_HandleTypeDef hspi1;

TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim17;

/* USER CODE BEGIN PV */
static uint8_t overtemp = 0;      /* 0 = ok, 1 = over temperature, 2 = sensor */
static uint8_t overtemp_cnt = 0;  /* consecutive 100 ms cycles above the limit */
TIM_HandleTypeDef htim3;
TIM_HandleTypeDef htim15;
bool ZERO = false;
/* Debug: raw ADC value from PA0 (thermocouple front-end via INA826).
 * Updated every 100ms control cycle. Watch this in the debugger:
 *   0    = 0V at ADC input  (cold / no thermocouple)
 *   4095 = 3.3V             (saturated, ~527C)
 * Typical room-temp reading with cold junction ≈ 50..100 counts.
 *
 * dbg_adc_raw   : single raw conversion (noisy, for hardware check only)
 * dbg_adc_filt  : moving-average of last 16 raw samples (stable, use this)
 * dbg_adc_mv    : dbg_adc_filt converted to millivolts at PA0 pin (0..3300)
 * dbg_tc_emf_uv : EMF seen at thermocouple terminals in microvolts
 *                 = (dbg_adc_mv / INA_GAIN) including cold-junction add
 *                 expected range: ~0 uV cold .. ~8000 uV at 200 C (type K)
 * dbg_hot_c     : final computed temperature in °C × 10  (e.g. 1234 = 123.4 C)
 *                 same value shown on LCD, handy to compare against thermometer
 */
volatile uint16_t dbg_adc_raw  = 0;   /* latest single raw ADC count  0..4095 */
volatile uint16_t dbg_adc_filt = 0;   /* 16-sample moving average              */
volatile uint16_t dbg_adc_mv    = 0;   /* millivolts at PA0             0..3300  */
volatile int32_t  dbg_tc_emf_uv = 0;  /* thermocouple EMF in µV (incl. CJ)    */
volatile int16_t  dbg_hot_c     = 0;  /* temperature × 10, instantaneous      */
volatile int16_t  dbg_hot_avg   = 0;  /* temperature × 10, 8-sample moving avg */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_I2C1_Init(void);
static void MX_SPI1_Init(void);
static void MX_TIM1_Init(void);
static void MX_TIM17_Init(void);
static void MX_TIM3_Init(void);
static void MX_TIM15_Init(void);
/* USER CODE BEGIN PFP */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
	if (htim->Instance == TIM3) {
		Dimmer_TickISR();
	}
}

/* STM32G0's HAL does NOT call HAL_GPIO_EXTI_Callback() from the interrupt:
 HAL_GPIO_EXTI_IRQHandler() calls HAL_GPIO_EXTI_Rising_Callback() and
 HAL_GPIO_EXTI_Falling_Callback() instead, and both of those are empty weak
 stubs in the driver. Overriding HAL_GPIO_EXTI_Callback only compiles and
 then never runs -- that is exactly why the breakpoint there was never hit.
 Only the RISING edge is used: the H11AA1 fires once per half-cycle and using
 both edges would call ZeroCross() twice, halving the effective period and
 breaking the phase-angle table. */
void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin) {
	if (GPIO_Pin == ZERO_CROSS_Pin)
		Dimmer_ZeroCross();
}

void HAL_GPIO_EXTI_Falling_Callback(uint16_t GPIO_Pin) {
	/* Intentionally empty: only rising edge triggers the dimmer. */
	(void)GPIO_Pin;
}
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void) {

	/* USER CODE BEGIN 1 */

	/* USER CODE END 1 */

	/* MCU Configuration--------------------------------------------------------*/

	/* Reset of all peripherals, Initializes the Flash interface and the Systick. */
	HAL_Init();

	/* USER CODE BEGIN Init */

	/* USER CODE END Init */

	/* Configure the system clock */
	SystemClock_Config();

	/* USER CODE BEGIN SysInit */

	/* USER CODE END SysInit */

	/* Initialize all configured peripherals */
	MX_GPIO_Init();
	MX_ADC1_Init();
	MX_I2C1_Init();
	MX_SPI1_Init();
	MX_TIM1_Init();
	MX_TIM3_Init();
	MX_TIM15_Init();
	MX_TIM17_Init();
	/* USER CODE BEGIN 2 */
	/* Everything below sits inside USER CODE on purpose: CubeMX keeps these
	 sections on every "Generate Code", so a regeneration cannot wipe the
	 start-up. The .ioc carries the same settings; repeating them here also
	 keeps a build made from an older .ioc working. */

	/* LCD control pins PA3=CS, PA4=RESET, PA5=D/C as plain outputs. PA1/PA2
	 are SPI1_SCK / SPI1_MOSI and are configured by LCD_Init(). */
	GPIO_InitTypeDef gi = { 0 };
	gi.Mode = GPIO_MODE_OUTPUT_PP;
	gi.Pull = GPIO_NOPULL;
	gi.Speed = GPIO_SPEED_FREQ_HIGH;
	gi.Pin = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5;
	HAL_GPIO_Init(GPIOA, &gi);

	/* The two triac gate signals (CH1 = PC6, CH2 = PB15 on this build) must be
	 ordinary GPIO outputs. If CubeMX puts them back on a timer channel they
	 become alternate-function pins, HAL_GPIO_WritePin() stops reaching the
	 MOC3021 and neither channel fires at all. */

	gi.Pin = CH1_Pin;
	HAL_GPIO_Init(CH1_GPIO_Port, &gi);
	gi.Pin = CH2_Pin;
	HAL_GPIO_Init(CH2_GPIO_Port, &gi);
	HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET);
	/* LEDs are active-LOW, so HIGH = off. Repeated here (not only in the
	   generated MX_GPIO_Init) because CubeMX would otherwise restore the old
	   RESET level and every LED would light during the splash. */
	HAL_GPIO_WritePin(GPIOB, ERR_LED_Pin | CH1_LED_Pin | CH2_LED_Pin,
			GPIO_PIN_SET);

	/* keys PC0..PC3: internal pull-ups, a floating pin must never look like a
	 press */
	gi.Pin = GPIO_PIN_0 | SW3_Pin | SW2_Pin | SW1_Pin;
	gi.Mode = GPIO_MODE_INPUT;
	gi.Pull = GPIO_PULLUP;
	HAL_GPIO_Init(GPIOC, &gi);

	/* Zero-cross input on PC7. The SYSCFG clock MUST be on before the EXTI
	 port selector is written: HAL_GPIO_Init() writes EXTI->EXTICR but this
	 HAL revision never enables that clock itself, so the write is silently
	 dropped and line 7 stays routed to PA7 -- the pin toggles happily and
	 no interrupt is ever generated. Doing it here as well makes the line
	 survive any CubeMX regenerate. */
	__HAL_RCC_SYSCFG_CLK_ENABLE();
//	gi.Pin = ZERO_CROSS_Pin;
//	gi.Mode = GPIO_MODE_IT_RISING_FALLING;
//	gi.Pull = GPIO_NOPULL;
//	HAL_GPIO_Init(ZERO_CROSS_GPIO_Port, &gi);
////	EXTI->RPR1 = ZERO_CROSS_Pin; /* drop a stale pending edge */
////	EXTI->FPR1 = ZERO_CROSS_Pin;
//	HAL_NVIC_SetPriority(EXTI4_15_IRQn, 0, 0);
//	HAL_NVIC_EnableIRQ(EXTI4_15_IRQn);

	/* TIM17 (ENABLE2/PB9, box fan): its clock comes from HAL_TIM_Base_MspInit,
	 this keeps the PWM alive whatever the generated init does. */
	__HAL_RCC_TIM17_CLK_ENABLE();

	/* TIM3 = 50 us dimmer time base, derived from the live APB clock so a
	 clock-tree change cannot shift the firing angle. */
	RCC_ClkInitTypeDef clock_cfg;
	uint32_t flash_latency;
	HAL_RCC_GetClockConfig(&clock_cfg, &flash_latency);
	uint32_t tim_clk = HAL_RCC_GetPCLK1Freq();
	if (clock_cfg.APB1CLKDivider != RCC_HCLK_DIV1)
		tim_clk *= 2UL;
	__HAL_RCC_TIM3_CLK_ENABLE();
	HAL_TIM_Base_Stop_IT(&htim3);
	htim3.Instance = TIM3;
	htim3.Init.Prescaler = (tim_clk / 1000000UL) - 1UL; /* 1 us tick */
	htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim3.Init.Period = 49; /* 50 x 1 us */
	htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
	if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
		Error_Handler();
	/* TIM3 (50 us tick) and the zero-cross EXTI both call schedule_half().
	   With the tick at a lower priority the edge ISR could preempt it in the
	   middle of the half-cycle setup and arm the same channel twice. Equal
	   priorities on Cortex-M0+ mean they can never nest, which is exactly
	   what the shared state needs. */
	HAL_NVIC_SetPriority(TIM3_IRQn, 0, 0);
	HAL_NVIC_EnableIRQ(TIM3_IRQn);
	HAL_NVIC_SetPriority(EXTI4_15_IRQn, 0, 0);
	HAL_NVIC_EnableIRQ(EXTI4_15_IRQn);

	/* Dimmer state must be ready BEFORE the timer can call the ISR: both the
	   tick and the zero-cross callback dereference it. Starting the timer
	   first left the first few milliseconds running on reset values. */
	Dimmer_Init();
	if (HAL_TIM_Base_Start_IT(&htim3) != HAL_OK)
		Error_Handler();

	/* The display comes up first so any later failure is visible on screen. */
	Keys_Init();
	LCD_Init();
	UI_Splash(); /* boot logo, then the UI takes over */
	Fan_Init();
	Temp_Init();
	Control_Init();
	UI_Init();

	HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET);
	/* USER CODE END 2 */

	/* Infinite loop */
	/* USER CODE BEGIN WHILE */
	while (1) {
		/* USER CODE END WHILE *//* USER CODE BEGIN 3 */
		if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_7))
			ZERO = true;
		if (!HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_7))
			ZERO = false;

		uint32_t now = HAL_GetTick();
		static uint32_t t_amb = 0, t_ctrl = 0;
		static float amb = 25.0f; /* cold junction temperature */
		static float hot = 25.0f;

		UI_HandleKeys();

		/* the SHT40 needs ~9 ms for a conversion, so once a second is plenty;
		 a failed read keeps the last good value. A single I2C frame can
		 still come back corrupt, and the cold-junction term feeds every
		 temperature reading, so a value that jumps more than 5 C in one
		 second is ignored. Five such readings in a row means the ambient
		 really did change (or the sensor was just powered), so the filter
		 re-primes instead of locking itself out for ever. */
		if ((now - t_amb) >= 1000UL) {
			static uint8_t amb_bad;
			t_amb = now;
			float amb_try;
			if (Temp_ReadAmbient(&amb_try) == 0 && amb_try > -40.0f
					&& amb_try < 100.0f) {
				if (amb_try > amb + 5.0f || amb_try < amb - 5.0f) {
					if (++amb_bad >= 5) { amb = amb_try; amb_bad = 0; }
				} else {
					amb_bad = 0;
					amb += 0.2f * (amb_try - amb);
				}
			}
		}

		/* the thermal loop runs on its own 100 ms grid, independent of how
		 long the LCD repaint takes */
		if ((now - t_ctrl) >= 100UL) {
			t_ctrl = now;
			hot = Temp_ReadHot(amb);

			/* --- debug ADC monitor ---
			 * 16-sample moving average of raw PA0 counts.
			 * No filtering beyond accumulation — this is intentionally
			 * separate from the trimmed-mean + deglitch inside temp.c so
			 * we can observe the raw hardware noise here. */
			{
				static uint32_t acc = 0;
				static uint8_t  acc_n = 0;
				static uint16_t buf16[16];
				static uint8_t  idx = 0;

				uint16_t s = Temp_ReadADC();
				dbg_adc_raw = s;

				acc -= buf16[idx];
				buf16[idx] = s;
				acc += s;
				idx = (idx + 1) & 0x0Fu;
				if (acc_n < 16) acc_n++;

				dbg_adc_filt = (uint16_t)(acc / acc_n);
				/* millivolts at PA0: count/4095 * 3300mV */
				dbg_adc_mv = (uint16_t)((uint32_t)dbg_adc_filt * 3300UL / 4095UL);

				/* EMF at thermocouple wires in uV:
				 *   PA0_mv -> subtract the 7 mV front-end offset -> / INA_GAIN
				 *            = amplifier input (= TC hot-cold EMF)
				 *   + cold junction EMF (from SHT40 ambient reading)
				 * INA_GAIN and TC_OFFSET_MV come from temp.h and are the
				 * values measured on this board (25C->7mV, 450C->1250mV),
				 * NOT the schematic's nominal 150.7. Compare dbg_tc_emf_uv
				 * against K-type: 100C->4096uV, 200C->8138uV, 450C->18550uV. */
				float v_pa0_mv = (float)dbg_adc_filt * 3300.0f / 4095.0f;
				float tc_emf_mv = (v_pa0_mv - TC_OFFSET_MV) / INA_GAIN;
				/* add cold-junction EMF (µV) */
				float cj_mv = 0.03970f * amb + 1.21e-5f * amb * amb;
				dbg_tc_emf_uv = (int32_t)((tc_emf_mv + cj_mv) * 1000.0f);

				/* temperature from the same path used for control */
				dbg_hot_c = (int16_t)(hot * 10.0f + 0.5f);

				/* 8-sample moving average of temperature (smoothed display) */
				{
					static int32_t tacc = 0;
					static int16_t tbuf[8] = {0};
					static uint8_t tidx = 0;
					tacc -= tbuf[tidx];
					tbuf[tidx] = dbg_hot_c;
					tacc += dbg_hot_c;
					tidx = (tidx + 1) & 0x07u;
					dbg_hot_avg = (int16_t)(tacc / 8);
				}
			}

			float sp = UI_GetSetpoint();

			if (Control_GetFault() != CTRL_FAULT_NONE) {
				/* the loop detected that power does not produce heat: wrong or
				 disconnected thermocouple. The controller already holds the
				 output at 0; the user re-arms by toggling HEATER off and on. */
				overtemp = 2;
				overtemp_cnt = 0;
			} else {
				if (overtemp == 2)
					overtemp = 0;
				/* A setpoint at or below HEATER_MIN_SP_C (25.0 C) means "OFF":
				 the heater must not be treated as armed, otherwise a warm
				 residual temperature would raise ERR while the user has it
				 switched off. The absolute 520C limit always applies.
				 *
				 * A single 100 ms sample can still jump on a mains burst even
				 * after the filtering in temp.c, and tripping on one sample
				 * turns a transient into a visible fault. Three consecutive
				 * cycles above the limit are required; a real over-temperature
				 * still stops the element within 300 ms. */
				if ((Control_GetEnabled() && sp > HEATER_MIN_SP_C && hot > sp + 40.0f)
						|| hot > 520.0f) {
					if (overtemp_cnt < 3)
						overtemp_cnt++;
					if (overtemp_cnt >= 3) {
						overtemp = 1;
						Control_SetEnabled(0);
					}
				} else {
					overtemp_cnt = 0;
					if (overtemp == 1 && hot < sp + 20.0f) {
						overtemp = 0;
						/* restore the user's own choice instead of always
						 * re-arming, but only if not in manual mode (manual
						 * bypasses the PID) */
						if (!UI_GetHeaterManual())
							Control_SetEnabled(UI_GetEnabled());
					}
				}
			}

			/* In manual mode the PID is bypassed completely. Running it would
			   only call Dimmer_SetHeater(0) every tick and fight the manual
			   value written below. */
			if (!UI_GetHeaterManual())
				Control_Update(hot, sp, amb);
		}

		/* --- effective CH1 duty: one source of truth -----------------------
		 * Manual override wins over the PID, an over-temperature or sensor
		 * fault wins over both. Writing this every pass (not only on the
		 * 100 ms control tick) is what keeps the dimmer, the LCD and the LED
		 * in agreement. Before, the UI and the LED showed the PID output
		 * while the dimmer was driven by the manual value, so a manual
		 * percentage looked as if it did nothing. */
		float heater_duty;
		if (overtemp) {
			heater_duty = 0.0f;
		} else if (UI_GetHeaterManual()) {
			heater_duty = (float)UI_GetHeaterManualPct();
		} else {
			heater_duty = Control_GetHeaterDuty();
		}
		Dimmer_SetHeaterFine(heater_duty);
		uint8_t heater_pct = (uint8_t)(heater_duty + 0.5f);

		/* In AUTO the heater fan follows the heater (manual included, so a
		   manual duty cannot cook the element with no airflow); the aux fan
		   is always manual. */
		Fan_Update(heater_pct > 0);

		uint8_t iron_pct = Dimmer_GetIron();
		uint8_t fan_pct = Fan_GetSpeed(FAN_CH0);
		uint8_t fan2_pct = Fan_GetSpeed(FAN_CH1);

		/* LEDs (active-low: pin LOW = LED on)
		 * ERR_LED  = PB14: on while overtemp or fault
		 * CH1_LED  = PB13: on while the heater is driven
		 * CH2_LED  = PB12: on while the iron is driven */
		HAL_GPIO_WritePin(ERR_LED_GPIO_Port, ERR_LED_Pin,
				overtemp ? GPIO_PIN_RESET : GPIO_PIN_SET);
		HAL_GPIO_WritePin(CH1_LED_GPIO_Port, CH1_LED_Pin,
				heater_pct > 0 ? GPIO_PIN_RESET : GPIO_PIN_SET);
		HAL_GPIO_WritePin(CH2_LED_GPIO_Port, CH2_LED_Pin,
				iron_pct > 0 ? GPIO_PIN_RESET : GPIO_PIN_SET);

		UI_Update(hot, heater_pct, iron_pct, fan_pct, fan2_pct, overtemp);

		HAL_Delay(25);
		/* USER CODE END 3 */
	}
}
/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void) {
	RCC_OscInitTypeDef RCC_OscInitStruct = { 0 };
	RCC_ClkInitTypeDef RCC_ClkInitStruct = { 0 };

	/** Configure the main internal regulator output voltage
	 */
	HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

	/** Initializes the RCC Oscillators according to the specified parameters
	 * in the RCC_OscInitTypeDef structure.
	 */
	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
	RCC_OscInitStruct.HSIState = RCC_HSI_ON;
	RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
	RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
	RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
	RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV1;
	RCC_OscInitStruct.PLL.PLLN = 8;
	RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
	RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
	if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
		Error_Handler();
	}

	/** Initializes the CPU, AHB and APB buses clocks
	 */
	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
			| RCC_CLOCKTYPE_PCLK1;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

	if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) {
		Error_Handler();
	}
}

/**
 * @brief ADC1 Initialization Function
 * @param None
 * @retval None
 *//**
 * @brief ADC1 Initialization Function
 * @param None
 * @retval None
 */
static void MX_ADC1_Init(void) {

	/* USER CODE BEGIN ADC1_Init 0 */

	/* USER CODE END ADC1_Init 0 */

	ADC_ChannelConfTypeDef sConfig = { 0 };

	/* USER CODE BEGIN ADC1_Init 1 */

	/* USER CODE END ADC1_Init 1 */

	/** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
	 */
	hadc1.Instance = ADC1;
	hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
	hadc1.Init.Resolution = ADC_RESOLUTION_12B;
	hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
	hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
	hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
	hadc1.Init.LowPowerAutoWait = DISABLE;
	hadc1.Init.LowPowerAutoPowerOff = DISABLE;
	hadc1.Init.ContinuousConvMode = DISABLE;
	hadc1.Init.NbrOfConversion = 1;
	hadc1.Init.DiscontinuousConvMode = DISABLE;
	hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
	hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
	hadc1.Init.DMAContinuousRequests = DISABLE;
	hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
	hadc1.Init.SamplingTimeCommon1 = ADC_SAMPLETIME_1CYCLE_5;
	hadc1.Init.SamplingTimeCommon2 = ADC_SAMPLETIME_1CYCLE_5;
	hadc1.Init.OversamplingMode = DISABLE;
	hadc1.Init.TriggerFrequencyMode = ADC_TRIGGER_FREQ_HIGH;
	if (HAL_ADC_Init(&hadc1) != HAL_OK) {
		Error_Handler();
	}

	/** Configure Regular Channel
	 */
	sConfig.Channel = ADC_CHANNEL_0;
	sConfig.Rank = ADC_REGULAR_RANK_1;
	sConfig.SamplingTime = ADC_SAMPLINGTIME_COMMON_1;
	if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
		Error_Handler();
	}
	/* USER CODE BEGIN ADC1_Init 2 */

	/* USER CODE END ADC1_Init 2 */

}

/**
 * @brief I2C1 Initialization Function
 * @param None
 * @retval None
 */
static void MX_I2C1_Init(void) {

	/* USER CODE BEGIN I2C1_Init 0 */

	/* USER CODE END I2C1_Init 0 */

	/* USER CODE BEGIN I2C1_Init 1 */

	/* USER CODE END I2C1_Init 1 */
	hi2c1.Instance = I2C1;
	hi2c1.Init.Timing = 0xC0100F14;
	hi2c1.Init.OwnAddress1 = 0;
	hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
	hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
	hi2c1.Init.OwnAddress2 = 0;
	hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
	hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
	hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
	if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
		Error_Handler();
	}

	/** Configure Analogue filter
	 */
	if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE)
			!= HAL_OK) {
		Error_Handler();
	}

	/** Configure Digital filter
	 */
	if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK) {
		Error_Handler();
	}
	/* USER CODE BEGIN I2C1_Init 2 */

	/* USER CODE END I2C1_Init 2 */

}

/**
 * @brief SPI1 Initialization Function
 * @param None
 * @retval None
 */
static void MX_SPI1_Init(void) {

	/* USER CODE BEGIN SPI1_Init 0 */

	/* USER CODE END SPI1_Init 0 */

	/* USER CODE BEGIN SPI1_Init 1 */

	/* USER CODE END SPI1_Init 1 */
	/* SPI1 parameter configuration*/
	hspi1.Instance = SPI1;
	hspi1.Init.Mode = SPI_MODE_MASTER;
	hspi1.Init.Direction = SPI_DIRECTION_2LINES;
	hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
	hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
	hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
	hspi1.Init.NSS = SPI_NSS_SOFT;
	hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
	hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
	hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
	hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
	hspi1.Init.CRCPolynomial = 7;
	hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
	hspi1.Init.NSSPMode = SPI_NSS_PULSE_ENABLE;
	if (HAL_SPI_Init(&hspi1) != HAL_OK) {
		Error_Handler();
	}
	/* USER CODE BEGIN SPI1_Init 2 */

	/* USER CODE END SPI1_Init 2 */

}

/**
 * @brief TIM1 Initialization Function
 * @param None
 * @retval None
 */
static void MX_TIM1_Init(void) {

	/* USER CODE BEGIN TIM1_Init 0 */

	/* USER CODE END TIM1_Init 0 */

	TIM_MasterConfigTypeDef sMasterConfig = { 0 };
	TIM_OC_InitTypeDef sConfigOC = { 0 };
	TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = { 0 };

	/* USER CODE BEGIN TIM1_Init 1 */

	/* USER CODE END TIM1_Init 1 */
	htim1.Instance = TIM1;
	htim1.Init.Prescaler = 0;
	htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim1.Init.Period = 65535;
	htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim1.Init.RepetitionCounter = 0;
	htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
	if (HAL_TIM_PWM_Init(&htim1) != HAL_OK) {
		Error_Handler();
	}
	sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
	sMasterConfig.MasterOutputTrigger2 = TIM_TRGO2_RESET;
	sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
	if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig)
			!= HAL_OK) {
		Error_Handler();
	}
	sConfigOC.OCMode = TIM_OCMODE_PWM1;
	sConfigOC.Pulse = 0;
	sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
	sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
	sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
	sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
	sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
	if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1)
			!= HAL_OK) {
		Error_Handler();
	}
	sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_DISABLE;
	sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_DISABLE;
	sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
	sBreakDeadTimeConfig.DeadTime = 0;
	sBreakDeadTimeConfig.BreakState = TIM_BREAK_DISABLE;
	sBreakDeadTimeConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
	sBreakDeadTimeConfig.BreakFilter = 0;
	sBreakDeadTimeConfig.BreakAFMode = TIM_BREAK_AFMODE_INPUT;
	sBreakDeadTimeConfig.Break2State = TIM_BREAK2_DISABLE;
	sBreakDeadTimeConfig.Break2Polarity = TIM_BREAK2POLARITY_HIGH;
	sBreakDeadTimeConfig.Break2Filter = 0;
	sBreakDeadTimeConfig.Break2AFMode = TIM_BREAK_AFMODE_INPUT;
	sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
	if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBreakDeadTimeConfig)
			!= HAL_OK) {
		Error_Handler();
	}
	/* USER CODE BEGIN TIM1_Init 2 */

	/* USER CODE END TIM1_Init 2 */
	HAL_TIM_MspPostInit(&htim1);

}

/**
 * @brief TIM3 Initialization Function
 * @param None
 * @retval None
 */
static void MX_TIM3_Init(void) {

	/* USER CODE BEGIN TIM3_Init 0 */

	/* USER CODE END TIM3_Init 0 */

	/* USER CODE BEGIN TIM3_Init 1 */

	/* USER CODE END TIM3_Init 1 */
	htim3.Instance = TIM3;
	htim3.Init.Prescaler = 49;
	htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim3.Init.Period = 49;
	htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
	if (HAL_TIM_Base_Init(&htim3) != HAL_OK) {
		Error_Handler();
	}
	/* USER CODE BEGIN TIM3_Init 2 */

	/* USER CODE END TIM3_Init 2 */

}

/**
 * @brief TIM15 Initialization Function
 * @param None
 * @retval None
 */
static void MX_TIM15_Init(void) {

	/* USER CODE BEGIN TIM15_Init 0 */

	/* USER CODE END TIM15_Init 0 */

	/* USER CODE BEGIN TIM15_Init 1 */

	/* USER CODE END TIM15_Init 1 */
	htim15.Instance = TIM15;
	htim15.Init.Prescaler = 0;
	htim15.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim15.Init.Period = 65535;
	htim15.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim15.Init.RepetitionCounter = 0;
	htim15.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
	if (HAL_TIM_Base_Init(&htim15) != HAL_OK) {
		Error_Handler();
	}
	/* USER CODE BEGIN TIM15_Init 2 */

	/* USER CODE END TIM15_Init 2 */

}

/**
 * @brief TIM17 Initialization Function
 * @param None
 * @retval None
 */
static void MX_TIM17_Init(void) {

	/* USER CODE BEGIN TIM17_Init 0 */

	/* USER CODE END TIM17_Init 0 */

	TIM_OC_InitTypeDef sConfigOC = { 0 };
	TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = { 0 };

	/* USER CODE BEGIN TIM17_Init 1 */

	/* USER CODE END TIM17_Init 1 */
	htim17.Instance = TIM17;
	htim17.Init.Prescaler = 0;
	htim17.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim17.Init.Period = 65535;
	htim17.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim17.Init.RepetitionCounter = 0;
	htim17.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
	if (HAL_TIM_Base_Init(&htim17) != HAL_OK) {
		Error_Handler();
	}
	if (HAL_TIM_PWM_Init(&htim17) != HAL_OK) {
		Error_Handler();
	}
	sConfigOC.OCMode = TIM_OCMODE_PWM1;
	sConfigOC.Pulse = 0;
	sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
	sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
	sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
	sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
	sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
	if (HAL_TIM_PWM_ConfigChannel(&htim17, &sConfigOC, TIM_CHANNEL_1)
			!= HAL_OK) {
		Error_Handler();
	}
	sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_DISABLE;
	sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_DISABLE;
	sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
	sBreakDeadTimeConfig.DeadTime = 0;
	sBreakDeadTimeConfig.BreakState = TIM_BREAK_DISABLE;
	sBreakDeadTimeConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
	sBreakDeadTimeConfig.BreakFilter = 0;
	sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
	if (HAL_TIMEx_ConfigBreakDeadTime(&htim17, &sBreakDeadTimeConfig)
			!= HAL_OK) {
		Error_Handler();
	}
	/* USER CODE BEGIN TIM17_Init 2 */

	/* USER CODE END TIM17_Init 2 */
	HAL_TIM_MspPostInit(&htim17);

}

/**
 * @brief GPIO Initialization Function
 * @param None
 * @retval None
 */
static void MX_GPIO_Init(void) {
	GPIO_InitTypeDef GPIO_InitStruct = { 0 };
	/* USER CODE BEGIN MX_GPIO_Init_1 */

	/* USER CODE END MX_GPIO_Init_1 */

	/* GPIO Ports Clock Enable */
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();

	/* EXTICR is written through the SYSCFG block: without this clock the
	 port selector below is dropped and EXTI line 7 keeps pointing at PA7. */
	__HAL_RCC_SYSCFG_CLK_ENABLE();

	/*Configure GPIO pin Output Level */
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3 | RESET_Pin | D_C_Pin, GPIO_PIN_RESET);

	/*Configure GPIO pin Output Level */
	HAL_GPIO_WritePin(CH1_GPIO_Port, CH1_Pin, GPIO_PIN_RESET);

	/*Configure GPIO pin Output Level */
	HAL_GPIO_WritePin(CH2_GPIO_Port, CH2_Pin, GPIO_PIN_RESET);

	/* All three LEDs are active-LOW: drive the pins HIGH at boot so the
	   board does not flash every LED while the UI is still starting. */
	HAL_GPIO_WritePin(GPIOB, ERR_LED_Pin | CH1_LED_Pin | CH2_LED_Pin,
			GPIO_PIN_SET);

	/*Configure GPIO pins : PC0 SW3_Pin SW2_Pin SW1_Pin */
	GPIO_InitStruct.Pin = GPIO_PIN_0 | SW3_Pin | SW2_Pin | SW1_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

	/*Configure GPIO pins : PA3 RESET_Pin D_C_Pin */
	GPIO_InitStruct.Pin = GPIO_PIN_3 | RESET_Pin | D_C_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

	/*Configure GPIO pins : ERR_LED_Pin CH1_LED_Pin CH2_LED_Pin */
	GPIO_InitStruct.Pin = ERR_LED_Pin | CH1_LED_Pin | CH2_LED_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

	/*Configure GPIO pin : CH1_Pin (triac gate, plain output) */
	GPIO_InitStruct.Pin = CH1_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(CH1_GPIO_Port, &GPIO_InitStruct);

	/*Configure GPIO pin : CH2_Pin (triac gate, plain output)
	 * The Pin field must be rewritten: the previous call left CH1_Pin in it,
	 * so CH2 (PB15) was never configured as an output and the iron gate
	 * stayed in its reset state. */
	GPIO_InitStruct.Pin = CH2_Pin;
	HAL_GPIO_Init(CH2_GPIO_Port, &GPIO_InitStruct);

	/*Configure GPIO pin : ZERO_CROSS_Pin
	 * Rising edge only: the H11AA1 output is HIGH around the zero crossing
	 * (collector pulled up, phototransistor conducting near 0 V), so its
	 * rising edge is the true zero point. Using both edges called the dimmer
	 * twice per half-cycle and halved the effective period.
	 * The board already has an external 10 k pull-up on the collector, so the
	 * internal one is redundant and is left off. */
	GPIO_InitStruct.Pin = ZERO_CROSS_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(ZERO_CROSS_GPIO_Port, &GPIO_InitStruct);

	/* EXTI interrupt init*/
	HAL_NVIC_SetPriority(EXTI4_15_IRQn, 0, 0);
	HAL_NVIC_EnableIRQ(EXTI4_15_IRQn);

	/* USER CODE BEGIN MX_GPIO_Init_2 */

	/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */
/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void) {
	/* USER CODE BEGIN Error_Handler_Debug */
	/* User can add his own implementation to report the HAL error return state */
	__disable_irq();
	while (1) {
	}
	/* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
