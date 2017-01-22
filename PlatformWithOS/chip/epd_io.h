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


#if !defined(EPD_IO_H)
#define EPD_IO_H 1

#define panel_on_pin  CSID4
#define border_pin    UART1_TX
#define discharge_pin UART1_RX
#define pwm_pin       CSID1
#define reset_pin     CSID7
#define busy_pin      CSID6
#define flash_cs_pin  CSID0

#define SPI_DEVICE    "/dev/spidev32766.0"
#define SPI_BPS       30000000

#endif
