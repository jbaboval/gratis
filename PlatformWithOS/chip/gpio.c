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


// For the BCM SOC Preipheral Manual register layout see:
//   http://www.raspberrypi.org/wp-content/uploads/2012/02/BCM2835-ARM-Peripherals.pdf
//
// also see: http://elinux.org/RPi_Low-level_peripherals
//
// Other items are difficult to determine accurately. Google search "Gert Doms" for some samples


#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <err.h>

#include "gpio.h"
#include "libsoc_debug.h"

board_config *board;
gpio *pins[2048];

static gpio *
get_pin(GPIO_pin_type pin_no) {

	if (pin_no >= 2048) {
		warn("PIN out of range");
		return NULL;
	}

	if (pins[pin_no] == NULL) {
		pins[pin_no] = libsoc_gpio_request(pin_no, LS_SHARED);
	}

	return pins[pin_no];
}

// set up access to the GPIO and PWM
bool GPIO_setup() {
//	libsoc_set_debug(1);

	board = libsoc_board_init();

	if (board == NULL) {
		warn("ERROR: Cannot initialize board pin database");
		return false;
	}

	return true;
}


// revoke access to GPIO and PWM
bool GPIO_teardown() {

	int i;

	for (i=0; i<2048; i++) {
		if (pins[i]) {
			libsoc_gpio_free(pins[i]);
		}
	}

	if (board) {
		libsoc_board_free(board);
	}
	return true;
}


void GPIO_mode(GPIO_pin_type pin_no, GPIO_mode_type mode) {

	gpio *pin = NULL;
        pin = get_pin(pin_no);

	if (pin == NULL) {
		return;
	}

	switch (mode) {
	default:
	case GPIO_INPUT:
		libsoc_gpio_set_direction(pin, INPUT);
		break;
	case GPIO_OUTPUT:
		libsoc_gpio_set_direction(pin, OUTPUT);
		break;
	case GPIO_PWM:  // only certain pins allowed
		warn("PWM not implemented");
		break;
	}
}


int GPIO_read(GPIO_pin_type pin_no) {
	gpio *pin = NULL;
	pin = get_pin(pin_no);

	return libsoc_gpio_get_level(pin);
}


void GPIO_write(GPIO_pin_type pin_no, int value) {
	gpio *pin = NULL;
	pin = get_pin(pin_no);

	libsoc_gpio_set_level(pin, value?HIGH:LOW);
}


// only affetct PWM if correct pin is addressed
void GPIO_pwm_write(GPIO_pin_type pin_no, uint32_t value) {
}
