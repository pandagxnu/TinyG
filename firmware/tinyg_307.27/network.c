/*
 * network.c - tinyg networking protocol
 * Part of TinyG project
 *
 * Copyright (c) 2010 - 2011 Alden S. Hart Jr.
 *
 * TinyG is free software: you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License as published by 
 * the Free Software Foundation, either version 3 of the License, 
 * or (at your option) any later version.
 *
 * TinyG is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 * See the GNU General Public License for details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with TinyG  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
//#include <util/delay.h>		//++++ for test purposes only

#include "xio.h"
#include "encoder.h"
#include "controller.h"
//#include "tinyg.h"

/*
 * Local Scope Functions and Data
 */

char nextchar(char c);

/*
 * net_init()
 */

void net_init() 
{
	return;
}

/* 
 * tg_repeater() - top-level controller.
 */

void tg_repeater()
{
//	uint8_t full_duplex = FALSE;
	uint8_t full_duplex = TRUE;
	unsigned char tx = 'Z';
	unsigned char rx;

	while (TRUE) {
		tx = nextchar(tx);
		xio_putc(XIO_DEV_RS485, tx);	// write to RS485 port
		if (full_duplex) {
			while ((rx = xio_getc(XIO_DEV_RS485)) == -1);	// blocking read
			xio_putc(XIO_DEV_USB, rx);	// echo RX to USB port
		} else {
			xio_putc(XIO_DEV_USB, tx);	// write TX to USB port
		}	
		en_toggle(1);
//		_delay_ms(10);
	}
}

/* 
 * tg_receiver()
 */

void tg_receiver()
{
//	tg_controller();	// this node executes gcode blocks received via RS485

//	int	getc_code = 0;
	int rx;

	xio_queue_RX_string_rs485("Z");		// simulate an RX char

	while (TRUE) {
		while ((rx = xio_getc(XIO_DEV_RS485)) == -1);
//		xio_putc(XIO_DEV_USB, rx);		// write to USB port
		xio_putc_rs485(rx, fdev_rs485);	// alternate form of above
		en_toggle(1);
	}
}

char nextchar(char c)
{
//	uint8_t cycle = FALSE;
	uint8_t cycle = TRUE;
	char n = c;

	if ((cycle) && ((n = ++c) > 'z')) {
		n = '0';
	}
	return (n);
}
