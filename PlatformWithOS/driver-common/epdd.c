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


#define VERSION 4

#define STR1(x) #x
#define STR(x) STR1(x)

#define FUSE_USE_VERSION 26

#include <stdint.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <err.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <b64/cdecode.h>
#include "gpio.h"
#include "spi.h"
#include "epd.h"
#include EPD_IO

#define BUFFER_SIZE 8192
#define SOCKET_PATH "/run/epdd"

#define ISTREQ(x, y) (strcasecmp(x, y) == 0)

static const char version_buffer[] = {STR(VERSION)};

#define VERSION_SIZE (sizeof(version_buffer) - sizeof((char)'\0'))

static const char *spi_device = SPI_DEVICE;        // default SPI device path
static const uint32_t spi_bps = SPI_BPS;           // default SPI device speed

// expect that external process changes this just before update command
// by sending text string e.g. shell:  echo 19 > /dev/epd/temperature
static int temperature = 19;                       // for external temperature compensation

#define MAKE_STRING_HELPER(s) #s
#define MAKE_STRING(s) MAKE_STRING_HELPER(s)

#define STR_CHIP MAKE_STRING(EPD_CHIP_VERSION)
#define STR_FILM MAKE_STRING(EPD_FILM_VERSION)

static const struct panel_struct {
	const char *key;
	const char *description;
	const EPD_size size;
	const int width;
	const int height;
	const int byte_count;
} panels[] = {
#if EPD_1_44_SUPPORT
	{"1.44", "EPD 1.44 128x96 COG " STR_CHIP " FILM " STR_FILM, EPD_1_44, 128, 96, 128 * 98 / 8},
#endif

#if EPD_1_9_SUPPORT
	{"1.9", "EPD 1.9 144x128 COG " STR_CHIP " FILM " STR_FILM, EPD_1_9, 144, 128, 144 * 128 / 8},
#endif

#if EPD_2_0_SUPPORT
	{"2.0", "EPD 2.0 200x96 COG " STR_CHIP " FILM " STR_FILM, EPD_2_0, 200, 96, 200 * 96 / 8},
#endif

#if EPD_2_6_SUPPORT
	{"2.6", "EPD 2.6 232x128 COG " STR_CHIP " FILM " STR_FILM, EPD_2_6, 232, 128, 232 * 128 / 8},
#endif

#if EPD_2_7_SUPPORT
	{"2.7", "EPD 2.7 264x176 COG " STR_CHIP " FILM " STR_FILM, EPD_2_7, 264, 176, 264 * 176 / 8},
#endif

	{NULL, NULL, 0, 0, 0, 0}  // must be last entry
};


// need to sync size with above (max of all sizes)
// this will be the next display
static char display_buffer[264 * 176 / 8];

// this is the current display
static char current_buffer[sizeof(display_buffer)];

static const struct panel_struct *panel = NULL;
static EPD_type *epd = NULL;
static SPI_type *spi = NULL;


// function prototypes
static void special_memcpy(char *d, const char *s, size_t size, bool bit_reversed, bool inverted);

static int
process_get_command(struct json_object *json_obj, int fd)
{
	json_object *parameter = NULL;

        if (!json_object_object_get_ex(json_obj, "parameter", &parameter)) {
		json_object_object_add(json_obj, "result",
		                       json_object_new_string("failure"));
		json_object_object_add(json_obj, "reason",
		                       json_object_new_string("Parameter missing"));
		return -EINVAL;
	}

	const char *param = json_object_get_string(parameter);

	if (strcmp("version", param) == 0) {
		json_object_object_add(json_obj, "value",
				       json_object_new_string(version_buffer));
	} else if (strcmp("panel", param) == 0) {
		json_object_object_add(json_obj, "value",
				       json_object_new_string(panel->description));
	} else if (strcmp("temperature", param) == 0) {
		int t = temperature;
		if (t < -99) {
			t = -99;
		} else if  (t > 99) {
			t = 99;
		}
		char t_buffer[16];
		snprintf(t_buffer, sizeof(t_buffer), "%3d\n", t);
		json_object_object_add(json_obj, "value",
				       json_object_new_string(t_buffer));
	} else {
                json_object_object_add(json_obj, "result",
                                       json_object_new_string("failure"));
                json_object_object_add(json_obj, "reason",
                                       json_object_new_string("Invalid Parameter"));
		return -ENOENT;
	}

	return 0;
}

#ifdef HEX
void hex_dump(char *data, int size, char *caption)
{
	int i; // index in data...
	int j; // index in line...
	char temp[8];
	char buffer[128];
	char *ascii;

	memset(buffer, 0, 128);

	printf("---------> %s <--------- (%d bytes from %p)\n", caption, size, data);

	// Printing the ruler...
	printf("        +0          +4          +8          +c            0   4   8   c   \n");

	// Hex portion of the line is 8 (the padding) + 3 * 16 = 52 chars long
	// We add another four bytes padding and place the ASCII version...
	ascii = buffer + 58;
	memset(buffer, ' ', 58 + 16);
	buffer[58 + 16] = '\n';
	buffer[58 + 17] = '\0';
	buffer[0] = '+';
	buffer[1] = '0';
	buffer[2] = '0';
	buffer[3] = '0';
	buffer[4] = '0';
	for (i = 0, j = 0; i < size; i++, j++)
	{
		if (j == 16)
		{
			printf("%s", buffer);
			memset(buffer, ' ', 58 + 16);

			sprintf(temp, "+%04x", i);
			memcpy(buffer, temp, 5);

			j = 0;
		}

		sprintf(temp, "%02x", 0xff & data[i]);
		memcpy(buffer + 8 + (j * 3), temp, 2);
		if ((data[i] > 31) && (data[i] < 127))
			ascii[j] = data[i];
		else
			ascii[j] = '.';
	}

	if (j != 0)
		printf("%s", buffer);
}
#endif

static int
process_image_command(struct json_object *json_obj, int fd)
{
	size_t len;
	bool inverted = false;
	bool bit_reversed = false;
	json_object *endian_obj = NULL;
        json_object *inverted_obj = NULL;
        json_object *data_obj = NULL;
	char buffer[BUFFER_SIZE];

        if (json_object_object_get_ex(json_obj, "endian", &endian_obj)) {
		const char *endian_str = json_object_get_string(endian_obj);
                if (strcasecmp("little", endian_str) == 0) {
			bit_reversed = true;
		}
	}

        if (json_object_object_get_ex(json_obj, "inverted", &inverted_obj)) {
        	inverted = json_object_get_boolean(inverted_obj);
        }

        if (!json_object_object_get_ex(json_obj, "data", &data_obj)) {
		json_object_object_add(json_obj, "result",
                                       json_object_new_string("failure"));
                json_object_object_add(json_obj, "reason",
                                       json_object_new_string("Missing 'data'"));
                return -ENOENT;
        }

        const char *data_str = json_object_get_string(data_obj);
        len = strlen(data_str);
        if (len > BUFFER_SIZE) {
            len = BUFFER_SIZE;
        }
        base64_decodestate b64state;
        base64_init_decodestate(&b64state);
        len = base64_decode_block(data_str, len, buffer, &b64state);

	if (len > 0) {
		special_memcpy(display_buffer, buffer, len, bit_reversed, inverted);
	}

	json_object_object_add(json_obj, "result",
		               json_object_new_string("success"));

	return 0;
}

static void *display_init(void) {

	if (!GPIO_setup()) {
		warn("GPIO_setup failed");
		goto done;
	}

	spi = SPI_create(spi_device, spi_bps);
	if (NULL == spi) {
		warn("SPI_setup failed");
		goto done_gpio;
	}

	GPIO_mode(panel_on_pin, GPIO_OUTPUT);
	GPIO_mode(border_pin, GPIO_OUTPUT);
	GPIO_mode(discharge_pin, GPIO_OUTPUT);
#if EPD_PWM_REQUIRED
	GPIO_mode(pwm_pin, GPIO_PWM);
#endif
	GPIO_mode(reset_pin, GPIO_OUTPUT);
	GPIO_mode(busy_pin, GPIO_INPUT);

	epd = EPD_create(panel->size,
			 panel_on_pin,
			 border_pin,
			 discharge_pin,
#if EPD_PWM_REQUIRED
			 pwm_pin,
#endif
			 reset_pin,
			 busy_pin,
			 spi);

	if (NULL == epd) {
		warn("EPD_setup failed");
		goto done_spi;
	}

	return (void *)epd;

	// release resources
//done_epd:
//        EPD_destroy(epd);
done_spi:
	SPI_destroy(spi);
done_gpio:
	GPIO_teardown();
done:
	return NULL;
}


static void display_destroy(void) {
	EPD_destroy(epd);
	SPI_destroy(spi);
	GPIO_teardown();
}

// bit reversed table
static const char reverse[256] = {
//	__00____01____02____03____04____05____06____07____08____09____0a____0b____0c____0d____0e____0f
	0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0, 0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
//	__10____11____12____13____14____15____16____17____18____19____1a____1b____1c____1d____1e____1f
	0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8, 0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
//	__20____21____22____23____24____25____26____27____28____29____2a____2b____2c____2d____2e____2f
	0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4, 0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
//	__30____31____32____33____34____35____36____37____38____39____3a____3b____3c____3d____3e____3f
	0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec, 0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
//	__40____41____42____43____44____45____46____47____48____49____4a____4b____4c____4d____4e____4f
	0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2, 0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
//	__50____51____52____53____54____55____56____57____58____59____5a____5b____5c____5d____5e____5f
	0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea, 0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
//	__60____61____62____63____64____65____66____67____68____69____6a____6b____6c____6d____6e____6f
	0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
//	__70____71____72____73____74____75____76____77____78____79____7a____7b____7c____7d____7e____7f
	0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee, 0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
//	__80____81____82____83____84____85____86____87____88____89____8a____8b____8c____8d____8e____8f
	0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1, 0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
//	__90____91____92____93____94____95____96____97____98____99____9a____9b____9c____9d____9e____9f
	0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9, 0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
//	__a0____a1____a2____a3____a4____a5____a6____a7____a8____a9____aa____ab____ac____ad____ae____af
	0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5, 0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
//	__b0____b1____b2____b3____b4____b5____b6____b7____b8____b9____ba____bb____bc____bd____be____bf
	0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed, 0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
//	__c0____c1____c2____c3____c4____c5____c6____c7____c8____c9____ca____cb____cc____cd____ce____cf
	0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3, 0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
//	__d0____d1____d2____d3____d4____d5____d6____d7____d8____d9____da____db____dc____dd____de____df
	0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb, 0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
//	__e0____e1____e2____e3____e4____e5____e6____e7____e8____e9____ea____eb____ec____ed____ee____ef
	0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7, 0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
//	__f0____f1____f2____f3____f4____f5____f6____f7____f8____f9____fa____fb____fc____fd____fe____ff
	0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef, 0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff
};


// copy buffer
static void special_memcpy(char *d, const char *s, size_t size, bool bit_reversed, bool inverted) {
	if (bit_reversed) {
		if (inverted) {
			for (size_t n = 0; n < size; ++n) {
				*d++ = reverse[(unsigned)(*s++)] ^ 0xff;
			}
		} else {
			for (size_t n = 0; n < size; ++n) {
				*d++ = reverse[(unsigned)(*s++)];
			}
		}
	} else if (inverted) {
		for (size_t n = 0; n < size; ++n) {
			*d++ = *s++ ^ 0xff;
		}
	} else {
		memcpy(d, s, size);
	}
}

static int
process_clear_command(struct json_object *json_obj, int fd)
{
	EPD_set_temperature(epd, temperature);
	EPD_begin(epd);
	if (EPD_OK != EPD_status(epd)) {
		warn("EPD_begin failed");
	}
	EPD_clear(epd);
	EPD_end(epd);

	memset(current_buffer, 0, sizeof(current_buffer));

        json_object_object_add(json_obj, "result",
                               json_object_new_string("success"));

	return 0;
}

static int
process_update_command(struct json_object *json_obj, int fd)
{
	EPD_set_temperature(epd, temperature);
	EPD_begin(epd);
	if (EPD_OK != EPD_status(epd)) {
		warn("EPD_begin failed");
	}
#if EPD_IMAGE_ONE_ARG
		EPD_image(epd, (const uint8_t *)display_buffer);
#elif EPD_IMAGE_TWO_ARG
		EPD_image(epd, (const uint8_t *)current_buffer, (const uint8_t *)display_buffer);
#else
#error "unsupported EPD_image() function"
#endif
	EPD_end(epd);

	memcpy(current_buffer, display_buffer, sizeof(display_buffer));

        json_object_object_add(json_obj, "result",
                               json_object_new_string("success"));

	return 0;
}

static int
process_blink_command(struct json_object *json_obj, int fd)
{
	EPD_set_temperature(epd, 29);
	EPD_begin(epd);
	if (EPD_OK != EPD_status(epd)) {
		warn("EPD_begin failed");
	}
	EPD_blink(epd, (const uint8_t *)display_buffer);
	EPD_end(epd);

	memcpy(current_buffer, display_buffer, sizeof(display_buffer));

        json_object_object_add(json_obj, "result",
                               json_object_new_string("success"));

	return 0;
}

static int
process_partial_command(struct json_object *json_obj, int fd)
{
	EPD_set_temperature(epd, temperature);
	EPD_begin(epd);
	if (EPD_OK != EPD_status(epd)) {
		warn("EPD_begin failed");
	}
#if EPD_PARTIAL_AVAILABLE
	// use partial update
	EPD_partial_image(epd, (const uint8_t *)current_buffer, (const uint8_t *)display_buffer);
#elif EPD_IMAGE_ONE_ARG
	// no partial so just normal display
	EPD_image(epd, (const uint8_t *)display_buffer);
#elif EPD_IMAGE_TWO_ARG
	// no partial so just normal display
	EPD_image(epd, (const uint8_t *)current_buffer, (const uint8_t *)display_buffer);
#else
#error "unsupported EPD_image() function"
#endif

	EPD_end(epd);

	memcpy(current_buffer, display_buffer, sizeof(display_buffer));
	

        json_object_object_add(json_obj, "result",
                               json_object_new_string("success"));

	return 0;
}

typedef struct json_command {
    const char *cmdStr;
    int (*command)(struct json_object *, int);
} json_command;

json_command commands[] = {
    { "clear",  process_clear_command },
    { "update", process_update_command },
    { "partial", process_partial_command },
    { "blink", process_blink_command },
    { "image", process_image_command },
    { "get", process_get_command },
    { NULL, NULL }
};

static void
process_json_command(struct json_object *json_obj, int fd)
{
    json_object *command = NULL;


    if (json_object_get_type(json_obj) != json_type_object) {
        fprintf(stderr, "Invalid json object\n");
        return;
    }

    if (json_object_object_get_ex(json_obj, "command", &command) &&
        json_object_get_type(command) == json_type_string) {

        const char *cmdStr = json_object_get_string(command);

        fprintf(stderr, "Processing '%s' command\n", cmdStr);

        for (unsigned i = 0; commands[i].cmdStr; i++) {
            if (ISTREQ(commands[i].cmdStr, cmdStr )) {
                commands[i].command(json_obj, fd);
                return;
            }
        }

        json_object_object_add(json_obj, "result",
                               json_object_new_string("invalid"));

        fprintf(stderr, "Invalid json command: %s\n", json_object_get_string(json_obj));
    }
}

static int option_processor(int argc, char **argv)
{
    int c;

    while (1) {
        int option_index = 0;
        static struct option long_options[] = {
            {"panel",      required_argument, 0, 'p'  },
            {"spi",        required_argument, 0, 's'  },
            {"version",    no_argument,       0, 'V'},
            {"help",       no_argument,       0, 'h'},
        };

        c = getopt_long(argc, argv, "Vhp:s:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
	     fprintf(stderr,
		     "usage: %s [options]\n"
		     "\n"
		     "general options:\n"
		     "    -h   --help      print help\n"
		     "    -V   --version   print version\n"
		     "\n"
		     "Panel options:\n"
		     "    --panel=NUM       same as '-opanel=SIZE'\n"
		     "    --spi=DEVICE      same as '-ospi=DEVICE'\n",
		     argv[0]);
	     exit(1);

        case 'V':
	     fprintf(stderr, "%s version %d\n", argv[0], VERSION);
	     exit(0);

        case 'p':
	     for (panel = panels; NULL != panel->key; ++panel) {
		     if (strcmp(panel->key, optarg) == 0) {
                         break;
		     }
	     }
             break;

        case 's':
	     spi_device = strdup(optarg);
             break;
        }
    }
    return 0;
}


int main(int argc, char *argv[])
{
    struct sockaddr_un addr;
    int localFd;
    int remoteFd;
    int res;
    char buffer[BUFFER_SIZE];
    struct json_object *json_obj;

    option_processor(argc, argv);

    memset(current_buffer, 0, sizeof(current_buffer));
    memset(display_buffer, 0, sizeof(display_buffer));

    unlink(SOCKET_PATH);

    localFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (localFd < 0) {
        perror("socket:");
        return (-1);
    }

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path));
    res = bind(localFd, (struct sockaddr *) &addr, SUN_LEN(&addr));
    if (res < 0) {
        perror("bind:");
        return (-1);
    }

    res = listen(localFd, 10);
    if (res < 0) {
        perror("listen:");
        return (-1);
    }

    display_init();

    while (1) {
        strncpy(buffer, "\n", sizeof(buffer));  // Inserts newline and fills the rest with nul chars.

        remoteFd = accept(localFd, NULL, NULL);

        fprintf(stderr, "Accepted connection...\n");

        if (remoteFd < 0) {
            perror("accept:");
            continue;
        }

        size_t offset = 0, len = 0;
        do {
            len = read(remoteFd, buffer + offset, BUFFER_SIZE - offset);
            if (len > 0) {
                offset += len;
            }
        } while (!(json_obj = json_tokener_parse(buffer)) && (offset < BUFFER_SIZE));

        if (json_obj && json_object_get_type(json_obj) == json_type_object) {
            process_json_command(json_obj, remoteFd);

            snprintf(buffer, BUFFER_SIZE, "%s", json_object_to_json_string(json_obj));
            json_object_put(json_obj);

        } else {
            fprintf(stderr, "Invalid object at %p (%d)\n", json_obj, offset);
            snprintf(&buffer[0], sizeof(buffer), "unknown\n");
        }

        if (write(remoteFd, &buffer[0], strlen(buffer)) < 0) {
            break;
        }

        if (close(remoteFd)) {
            perror("close:");
        }
    }

    display_destroy();

    if (close(localFd)) {
        perror("close:");
    }
}
