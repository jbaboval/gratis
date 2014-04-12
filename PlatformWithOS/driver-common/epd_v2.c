// Copyright 2013 Pervasive Displays, Inc.
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


#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <err.h>
#include <time.h>
#include <signal.h>

#include "gpio.h"
#include "spi.h"
#include "epd_v2.h"

// delays - more consistent naming
#define Delay_ms(ms) usleep(1000 * (ms))
#define Delay_us(us) usleep(us)
#define LOW 0
#define HIGH 1
#define digitalRead(pin) GPIO_read(pin)
#define digitalWrite(pin, value) GPIO_write(pin, value)


// inline arrays
#define ARRAY(type, ...) ((type[]){__VA_ARGS__})
#define CU8(...) (ARRAY(const uint8_t, __VA_ARGS__))

// types
typedef enum {           // Image pixel -> Display pixel
	EPD_inverse,     // B -> W, W -> B (New Image)
	EPD_normal       // B -> B, W -> W (New Image)
} EPD_stage;


// function prototypes

static void power_off(EPD_type *epd);

static void frame_fixed_timed(EPD_type *epd, uint8_t fixed_value, long stage_time);
static void frame_fixed_13(EPD_type *epd, uint8_t value, EPD_stage stage);
static void frame_data_13(EPD_type *epd, const uint8_t *image, EPD_stage stage);
static void frame_stage2(EPD_type *epd);
static void one_line(EPD_type *epd, uint16_t line, const uint8_t *data, uint8_t fixed_value,
		     EPD_stage stage, uint8_t border_byte);

// type for temperature compensation
typedef struct {
	uint16_t stage1_repeat;
	uint16_t stage1_step;
	uint16_t stage1_block;
	uint16_t stage2_repeat;
	uint16_t stage2_t1;
	uint16_t stage2_t2;
	uint16_t stage3_repeat;
	uint16_t stage3_step;
	uint16_t stage3_block;
} compensation_type;

// panel configuration
struct EPD_struct {
	int EPD_Pin_PANEL_ON;
	int EPD_Pin_BORDER;
	int EPD_Pin_DISCHARGE;
	int EPD_Pin_RESET;
	int EPD_Pin_BUSY;

	EPD_size size;
	int lines_per_display;
	int dots_per_line;
	int bytes_per_line;
	int bytes_per_scan;

	EPD_error status;

	const uint8_t *channel_select;
	size_t channel_select_length;

	const compensation_type *compensation;
	uint16_t temperature_offset;

	uint8_t *line_buffer;
	size_t line_buffer_size;

	timer_t timer;
	SPI_type *spi;
};


EPD_type *EPD_create(EPD_size size,
		     int panel_on_pin,
		     int border_pin,
		     int discharge_pin,
		     int reset_pin,
		     int busy_pin,
		     SPI_type *spi) {

	// create a polled timer
	timer_t timer;
	struct sigevent event;
	event.sigev_notify = SIGEV_NONE;

	if (-1 == timer_create(CLOCK_REALTIME, &event, &timer)) {
		warn("falled to create timer");
		return NULL;
	}

	// allocate memory
	EPD_type *epd = malloc(sizeof(EPD_type));
	if (NULL == epd) {
		warn("falled to allocate EPD structure");
		return NULL;
	}

	epd->spi = spi;
	epd->timer = timer;

	epd->EPD_Pin_PANEL_ON = panel_on_pin;
	epd->EPD_Pin_BORDER = border_pin;
	epd->EPD_Pin_DISCHARGE = discharge_pin;
	epd->EPD_Pin_RESET = reset_pin;
	epd->EPD_Pin_BUSY = busy_pin;

	epd->size = size;
	epd->lines_per_display = 96;
	epd->dots_per_line = 128;
	epd->bytes_per_line = 128 / 8;
	epd->bytes_per_scan = 96 / 4;

	EPD_set_temperature(epd, 25);

	// display size dependant items
	{
		static uint8_t cs[] = {0x72, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xff, 0x00};
		epd->channel_select = cs;
		epd->channel_select_length = sizeof(cs);
	}

	// set up size structure
	switch (size) {
	default:
	case EPD_1_44:  // default so no change
		break;

	case EPD_2_0: {
		epd->lines_per_display = 96;
		epd->dots_per_line = 200;
		epd->bytes_per_line = 200 / 8;
		epd->bytes_per_scan = 96 / 4;
		static uint8_t cs[] = {0x72, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xe0, 0x00};
		epd->channel_select = cs;
		epd->channel_select_length = sizeof(cs);
		break;
	}

	case EPD_2_7: {
		epd->lines_per_display = 176;
		epd->dots_per_line = 264;
		epd->bytes_per_line = 264 / 8;
		epd->bytes_per_scan = 176 / 4;
		static uint8_t cs[] = {0x72, 0x00, 0x00, 0x00, 0x7f, 0xff, 0xfe, 0x00, 0x00};
		epd->channel_select = cs;
		epd->channel_select_length = sizeof(cs);
		break;
	}
	}

	// buffer for frame line
	epd->line_buffer_size = 2 * epd->bytes_per_line + epd->bytes_per_scan
		+ 3; // command byte, border byte and filler byte

	epd->line_buffer = malloc(epd->line_buffer_size);
	if (NULL == epd->line_buffer) {
		free(epd);
		warn("falled to allocate EPD line buffer");
		return NULL;
	}

	return epd;
}


// deallocate memory
void EPD_destroy(EPD_type *epd) {
	if (NULL == epd) {
		return;
	}
	if (NULL != epd->line_buffer) {
		free(epd->line_buffer);
	}
	free(epd);
}


// read current status
EPD_error EPD_status(EPD_type *epd) {
	return epd->status;
}


//
void EPD_begin(EPD_type *epd) {

	// assume OK
	epd->status = EPD_OK;

	// power up sequence
	digitalWrite(epd->EPD_Pin_RESET, LOW);
	digitalWrite(epd->EPD_Pin_PANEL_ON, LOW);
	digitalWrite(epd->EPD_Pin_DISCHARGE, LOW);
	digitalWrite(epd->EPD_Pin_BORDER, LOW);

	SPI_on(epd->spi);

	Delay_ms(5);
	digitalWrite(epd->EPD_Pin_PANEL_ON, HIGH);
	Delay_ms(10);

	digitalWrite(epd->EPD_Pin_RESET, HIGH);
	digitalWrite(epd->EPD_Pin_BORDER, HIGH);
	Delay_ms(5);

	digitalWrite(epd->EPD_Pin_RESET, LOW);
	Delay_ms(5);

	digitalWrite(epd->EPD_Pin_RESET, HIGH);
	Delay_ms(5);

	// wait for COG to become ready
	while (HIGH == digitalRead(epd->EPD_Pin_BUSY)) {
		Delay_us(10);
	}

	// read the COG ID
	uint8_t receive_buffer[2];
	SPI_read(epd->spi, CU8(0x71, 0x00), receive_buffer, sizeof(receive_buffer));
	SPI_read(epd->spi, CU8(0x71, 0x00), receive_buffer, sizeof(receive_buffer));
	int cog_id = receive_buffer[1];
	if (0x02 != (0x0f & cog_id)) {
		epd->status = EPD_UNSUPPORTED_COG;
		power_off(epd);
		return;
	}

	// Disable OE
	SPI_send(epd->spi, CU8(0x70, 0x02), 2);
	SPI_send(epd->spi, CU8(0x72, 0x40), 2);

	// check breakage
	SPI_send(epd->spi, CU8(0x70, 0x0f), 2);
	SPI_read(epd->spi, CU8(0x73, 0x00), receive_buffer, sizeof(receive_buffer));
	int broken_panel = receive_buffer[1];
	if (0x00 == (0x80 & broken_panel)) {
		epd->status = EPD_PANEL_BROKEN;
		power_off(epd);
		return;
	}

	// power saving mode
	SPI_send(epd->spi, CU8(0x70, 0x0b), 2);
	SPI_send(epd->spi, CU8(0x72, 0x02), 2);

	// channel select
	SPI_send(epd->spi, CU8(0x70, 0x01), 2);
	SPI_send(epd->spi, epd->channel_select, epd->channel_select_length);

	// high power mode osc
	SPI_send(epd->spi, CU8(0x70, 0x07), 2);
	SPI_send(epd->spi, CU8(0x72, 0xd1), 2);

	// power setting
	SPI_send(epd->spi, CU8(0x70, 0x08), 2);
	SPI_send(epd->spi, CU8(0x72, 0x02), 2);

	// Vcom level
	SPI_send(epd->spi, CU8(0x70, 0x09), 2);
	SPI_send(epd->spi, CU8(0x72, 0xc2), 2);

	// power setting
	SPI_send(epd->spi, CU8(0x70, 0x04), 2);
	SPI_send(epd->spi, CU8(0x72, 0x03), 2);

	// driver latch on
	SPI_send(epd->spi, CU8(0x70, 0x03), 2);
	SPI_send(epd->spi, CU8(0x72, 0x01), 2);

	// driver latch off
	SPI_send(epd->spi, CU8(0x70, 0x03), 2);
	SPI_send(epd->spi, CU8(0x72, 0x00), 2);

	Delay_ms(5);

	bool dc_ok = false;

	for (int i = 0; i < 4; ++i) {
		// charge pump positive voltage on - VGH/VDL on
		SPI_send(epd->spi, CU8(0x70, 0x05), 2);
		SPI_send(epd->spi, CU8(0x72, 0x01), 2);

		Delay_ms(240);

		// charge pump negative voltage on - VGL/VDL on
		SPI_send(epd->spi, CU8(0x70, 0x05), 2);
		SPI_send(epd->spi, CU8(0x72, 0x03), 2);

		Delay_ms(40);

		// charge pump Vcom on - Vcom driver on
		SPI_send(epd->spi, CU8(0x70, 0x05), 2);
		SPI_send(epd->spi, CU8(0x72, 0x0f), 2);

		Delay_ms(40);

		// check DC/DC
		SPI_send(epd->spi, CU8(0x70, 0x0f), 2);
		SPI_read(epd->spi, CU8(0x73, 0x00), receive_buffer, sizeof(receive_buffer));
		int dc_state = receive_buffer[1];
		if (0x40 == (0x40 & dc_state)) {
			dc_ok = true;
			break;
		}
	}
	if (!dc_ok) {
		epd->status = EPD_DC_FAILED;
		power_off(epd);
		return;
	}

	// output enable to disable
	SPI_send(epd->spi, CU8(0x70, 0x02), 2);
	SPI_send(epd->spi, CU8(0x72, 0x40), 2);

	SPI_off(epd->spi);
}


void EPD_end(EPD_type *epd) {

	// dummy line and border
	if (EPD_2_7 == epd->size) {
		// only for 2.70" EPD
		Delay_ms(25);
		digitalWrite(epd->EPD_Pin_BORDER, LOW);
		Delay_ms(250);
		digitalWrite(epd->EPD_Pin_BORDER, HIGH);

	} else {

		// for 2.00"
		one_line(epd, 0x7fffu, 0, 0x00, EPD_normal, 0xff);
		Delay_ms(40);
		one_line(epd, 0x7fffu, 0, 0x00, EPD_normal, 0xaa);
		Delay_ms(200);
		one_line(epd, 0x7fffu, 0, 0x00, EPD_normal, 0x00);
		Delay_ms(25);
	}

	SPI_on(epd->spi);

	// check DC/DC
	uint8_t receive_buffer[2];
	SPI_send(epd->spi, CU8(0x70, 0x0f), 2);
	SPI_read(epd->spi, CU8(0x73, 0x00), receive_buffer, 2);
	int dc_state = receive_buffer[1];
	if (0x40 != (0x40 & dc_state)) {
		epd->status = EPD_DC_FAILED;
		power_off(epd);
		return;
	}

	// latch reset turn on
	SPI_send(epd->spi, CU8(0x70, 0x03), 2);
	SPI_send(epd->spi, CU8(0x72, 0x01), 2);

	// output enable off
	SPI_send(epd->spi, CU8(0x70, 0x02), 2);
	SPI_send(epd->spi, CU8(0x72, 0x05), 2);

	// power off positive charge pump
	SPI_send(epd->spi, CU8(0x70, 0x05), 2);
	SPI_send(epd->spi, CU8(0x72, 0x0e), 2);

	// power off Vcom charge pump
	SPI_send(epd->spi, CU8(0x70, 0x05), 2);
	SPI_send(epd->spi, CU8(0x72, 0x02), 2);

	// power off all charge pumps
	SPI_send(epd->spi, CU8(0x70, 0x05), 2);
	SPI_send(epd->spi, CU8(0x72, 0x00), 2);

	// turn of osc
	SPI_send(epd->spi, CU8(0x70, 0x07), 2);
	SPI_send(epd->spi, CU8(0x72, 0x0d), 2);

	// discharge internal on
	SPI_send(epd->spi, CU8(0x70, 0x04), 2);
	SPI_send(epd->spi, CU8(0x72, 0x83), 2);

	Delay_ms(120);

	// discharge internal off
	SPI_send(epd->spi, CU8(0x70, 0x04), 2);
	SPI_send(epd->spi, CU8(0x72, 0x00), 2);

	power_off(epd);
}


static void power_off(EPD_type *epd) {

	// turn of power and all signals
	digitalWrite(epd->EPD_Pin_RESET, LOW);
	digitalWrite(epd->EPD_Pin_PANEL_ON, LOW);
	digitalWrite(epd->EPD_Pin_BORDER, LOW);

	// ensure SPI MOSI and CLOCK are Low before CS Low
	SPI_off(epd->spi);

	// pulse discharge pin
	for (int i = 0; i < 10; ++i) {
		Delay_ms(10);
		digitalWrite(epd->EPD_Pin_DISCHARGE, HIGH);
		Delay_ms(10);
		digitalWrite(epd->EPD_Pin_DISCHARGE, LOW);
	}
}


void EPD_set_temperature(EPD_type *epd, int temperature) {
	static const compensation_type compensation_144[3] = {
		{ 2, 6, 42,   4, 392, 392,   2, 6, 42 },  //  0 ... 10 Celcius
		{ 4, 2, 16,   4, 155, 155,   4, 2, 16 },  // 10 ... 40 Celcius
		{ 4, 2, 16,   4, 155, 155,   4, 2, 16 }   // 40 ... 50 Celcius
	};

	static const compensation_type compensation_200[3] = {
		{ 2, 6, 42,   4, 392, 392,   2, 6, 42 },  //  0 ... 10 Celcius
		{ 2, 2, 48,   4, 196, 196,   2, 2, 48 },  // 10 ... 40 Celcius
		{ 4, 2, 48,   4, 196, 196,   4, 2, 48 }   // 40 ... 50 Celcius
	};

	static const compensation_type compensation_270[3] = {
		{ 2, 8, 64,   4, 392, 392,   2, 8, 64 },  //  0 ... 10 Celcius
		{ 2, 8, 64,   4, 196, 196,   2, 8, 64 },  // 10 ... 40 Celcius
		{ 4, 8, 64,   4, 196, 196,   4, 8, 64 }   // 40 ... 50 Celcius
	};

	if (temperature < 10) {
		epd->temperature_offset = 0;
	} else if (temperature > 40) {
		epd->temperature_offset = 2;
	} else {
		epd->temperature_offset = 1;
	}
	switch (epd->size) {
	default:
	case EPD_1_44:
		epd->compensation = &compensation_144[epd->temperature_offset];
		break;

	case EPD_2_0: {
		epd->compensation = &compensation_200[epd->temperature_offset];
		break;
	}

	case EPD_2_7: {
		epd->compensation = &compensation_270[epd->temperature_offset];
		break;
	}
	}
}

// clear display (anything -> white)
void EPD_clear(EPD_type *epd) {
	frame_fixed_13(epd, 0xff, EPD_inverse);
	frame_stage2(epd);
	frame_fixed_13(epd, 0xaa, EPD_normal);
}

// change from old image to new image
void EPD_image(EPD_type *epd, const uint8_t *image) {
	frame_data_13(epd, image, EPD_inverse);
	frame_stage2(epd);
	frame_data_13(epd, image, EPD_normal);
}


// internal functions
// ==================

// One frame of data is the number of lines * rows. For example:
// The 1.44” frame of data is 96 lines * 128 dots.
// The 2” frame of data is 96 lines * 200 dots.
// The 2.7” frame of data is 176 lines * 264 dots.

// the image is arranged by line which matches the display size
// so smallest would have 96 * 32 bytes

static void frame_fixed_timed(EPD_type *epd, uint8_t fixed_value, long stage_time) {
	struct itimerspec its;
	its.it_value.tv_sec = stage_time / 1000;
	its.it_value.tv_nsec = (stage_time % 1000) * 1000000;
	its.it_interval.tv_sec = 0;
	its.it_interval.tv_nsec = 0;

	if (-1 == timer_settime(epd->timer, 0, &its, NULL)) {
		err(1, "timer_settime failed");
	}
	do {
		for (uint8_t line = 0; line < epd->lines_per_display ; ++line) {
			one_line(epd, line, 0, fixed_value, EPD_normal, 0x00);
		}

		if (-1 == timer_gettime(epd->timer, &its)) {
			err(1, "timer_gettime failed");
		}
	} while (its.it_value.tv_sec > 0 && its.it_value.tv_nsec > 0);
}


static void frame_fixed_13(EPD_type *epd, uint8_t value, EPD_stage stage) {

	int repeat;
	int step;
	int block;
	if (EPD_inverse == stage) {  // stage 1
		repeat = epd->compensation->stage1_repeat;
		step = epd->compensation->stage1_step;
		block = epd->compensation->stage1_block;
	} else {                     // stage 3
		repeat = epd->compensation->stage3_repeat;
		step = epd->compensation->stage3_step;
		block = epd->compensation->stage3_block;
	}

	int total_lines = epd->lines_per_display;

	for (int n = 0; n < repeat; ++n) {

		for (int line = step - block; line < total_lines + step; line += step) {
			for (int offset = 0; offset < block; ++offset) {
				int pos = line + offset;
				if (pos < 0 || pos > total_lines) {
					one_line(epd, 0x7fffu, 0, 0x00, EPD_normal, 0x00);
				} else if (0 == offset && n == repeat - 1) {
					one_line(epd, pos, 0, 0x00, EPD_normal, 0x00);
				} else {
					one_line(epd, pos, 0, value, stage, 0x00);
				}
			}
		}
	}
}


static void frame_data_13(EPD_type *epd, const uint8_t *image, EPD_stage stage) {

	int repeat;
	int step;
	int block;
	if (EPD_inverse == stage) {  // stage 1
		repeat = epd->compensation->stage1_repeat;
		step = epd->compensation->stage1_step;
		block = epd->compensation->stage1_block;
	} else {                     // stage 3
		repeat = epd->compensation->stage3_repeat;
		step = epd->compensation->stage3_step;
		block = epd->compensation->stage3_block;
	}

	int total_lines = epd->lines_per_display;

	for (int n = 0; n < repeat; ++n) {

		for (int line = step - block; line < total_lines + step; line += step) {
			for (int offset = 0; offset < block; ++offset) {
				int pos = line + offset;
				if (pos < 0 || pos > total_lines) {
					one_line(epd, 0x7fffu, 0, 0x00, EPD_normal, 0x00);
				} else if (0 == offset && n == repeat - 1) {
					one_line(epd, pos, 0, 0x00, EPD_normal, 0x00);
				} else {
					one_line(epd, pos, &image[pos * epd->bytes_per_line], 0, stage, 0x00);
				}
			}
		}
	}
}


static void frame_stage2(EPD_type *epd) {
	for (int i = 0; i < epd->compensation->stage2_repeat; ++i) {
		frame_fixed_timed(epd, 0xff, epd->compensation->stage2_t1);
		frame_fixed_timed(epd, 0xaa, epd->compensation->stage2_t2);
	}
}


static void one_line(EPD_type *epd, uint16_t line, const uint8_t *data, uint8_t fixed_value,
	  EPD_stage stage, uint8_t border_byte) {

	SPI_on(epd->spi);

	// send data
	SPI_send(epd->spi, CU8(0x70, 0x0a), 2);

	Delay_us(10);

	// send data command byte
	uint8_t *p = epd->line_buffer;
	*p++ = 0x72;

	// border byte
	*p++ = border_byte;

	// odd pixels
	for (uint16_t b = epd->bytes_per_line; b > 0; --b) {
		if (0 != data) {
			uint8_t pixels = data[b - 1] & 0x55;
			switch(stage) {
			case EPD_inverse:      // B -> W, W -> B
				pixels = 0xaa | (pixels ^ 0x55);
				break;
			case EPD_normal:       // B -> B, W -> W
				pixels = 0xaa | pixels;
				break;
			}
			*p++ = pixels;
		} else {
			*p++ = fixed_value;
		}
	}

	// scan line
	int scan_pos = (epd->lines_per_display - line - 1) / 4;
	int scan_shift = 2 * (line & 0x03);
	for (uint16_t b = 0; b < epd->bytes_per_scan; ++b) {
		if (scan_pos == b) {
			*p++ = 0x03 << scan_shift;
		} else {
			*p++ = 0x00;
		}
	}

	// even pixels
	for (uint16_t b = 0; b < epd->bytes_per_line; ++b) {
		if (0 != data) {
			uint8_t pixels = data[b] & 0xaa;
			switch(stage) {
			case EPD_inverse:      // B -> W, W -> B (Current Image)
				pixels = 0xaa | ((pixels ^ 0xaa) >> 1);
				break;
			case EPD_normal:       // B -> B, W -> W (New Image)
				pixels = 0xaa | (pixels >> 1);
				break;
			}
			uint8_t p1 = (pixels >> 6) & 0x03;
			uint8_t p2 = (pixels >> 4) & 0x03;
			uint8_t p3 = (pixels >> 2) & 0x03;
			uint8_t p4 = (pixels >> 0) & 0x03;
			*p++ = (p1 << 0) | (p2 << 2) | (p3 << 4) | (p4 << 6);
		} else {
			*p++ = fixed_value;
		}
	}

	// send the accumulated line buffer
	SPI_send(epd->spi, epd->line_buffer, p - epd->line_buffer);

	// output data to panel
	SPI_send(epd->spi, CU8(0x70, 0x02), 2);
	SPI_send(epd->spi, CU8(0x72, 0x2f), 2);

	SPI_off(epd->spi);
}
