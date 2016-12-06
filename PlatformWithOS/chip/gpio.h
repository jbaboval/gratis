// Copyright 2013-2015 Pervasive Displays, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at:
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
// express or implied.  See the License for the specific language
// governing permissions and limitations under the License.


#if !defined(GPIO_H)
#define GPIO_H 1

#include <stdbool.h>
#include <stdint.h>

#include "libsoc_board.h"
#include "libsoc_gpio.h"
#include "libsoc_debug.h"

// pin types
typedef enum {

	PWM0       = 34,
	UART1_TX   = 195,
	UART1_RX   = 196,

	CSID0      = 132,
	CSID1      = 133,
	CSID2      = 134,
	CSID3      = 135,
	CSID4      = 136,
	CSID5      = 137,
	CSID6      = 138,
	CSID7      = 139,


	XIO_P0     = 1016,
	XIO_P1     = 1017,
	XIO_P2     = 1018,
	XIO_P3     = 1019,
	XIO_P4     = 1020,
	XIO_P5     = 1021,
	XIO_P6     = 1022,
	XIO_P7     = 1023,

} GPIO_pin_type;


// GPIO modes
typedef enum {
	GPIO_INPUT,   // as input
	GPIO_OUTPUT,  // as output
	GPIO_PWM      // as PWM output (only for P1_12
} GPIO_mode_type;


// functions
// =========

// enable GPIO system (maps device registers)
// return false if failure
bool GPIO_setup();

// release mapped device registers
bool GPIO_teardown();

// set a mode for a given GPIO pin
void GPIO_mode(GPIO_pin_type pin, GPIO_mode_type mode);

// return a value (0/1) for a given input pin
int GPIO_read(GPIO_pin_type pin);

// set or clear a given output pin
void GPIO_write(GPIO_pin_type pin, int value);

// set the PWM ration 0..1023 for hardware PWM pin (GPIO_P1_12)
void GPIO_pwm_write(GPIO_pin_type pin, uint32_t value);


#endif
