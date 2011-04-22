/** \file
 *	\brief scd.c source file for Smart Card Detective
 *
 *	This file implements the main application of the Smart Card Detective
 *
 *  It uses the functions defined in scd_hal.h and emv.h to communicate
 *  with the ICC and Terminal and the functions from scd_io.h to
 *  communicate with the LCD, buttons and LEDs 
 *
 *  Copyright (C) 2011 Omar Choudary (osc22@cam.ac.uk)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

// #define F_CPU 16000000UL  		  // Not needed if defined in Makefile

// this is needed for the delay on the new avr-libc-1.7.0
#ifndef __DELAY_BACKWARD_COMPATIBLE__
#define __DELAY_BACKWARD_COMPATIBLE__
#endif

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <avr/eeprom.h>
#include <string.h>
#include <util/delay.h>
#include <stdlib.h>
#include <avr/wdt.h> 

#include "apps.h"
#include "emv.h"
#include "scd_hal.h"
#include "scd_io.h"
#include "scd.h"
#include "utils.h"
#include "emv_values.h"
#include "scd_values.h"
#include "VirtualSerial.h"
#include "serial.h"

/// Set this to 1 to enable LCD functionality
#define LCD_ENABLED 1			

/// Set this to 1 to enable debug mode
#define DEBUG 0

/// Set this to 1 to enable card presence interrupt
#define ICC_PRES_INT_ENABLE 1 		

// If there are any assembler routines, they can be used
// by declaring them as below
// extern void StartClkICC(void);

/* Functions used in the main program */
//static inline void StopTerminal(const char *string);


/* Static variables */
#if LCD_ENABLED
static char* strATRSent = "ATR Sent";
static char* strError = "Error   Ocurred";
static char* strDataSent = "Data    Sent";
static char* strScroll = "BC to   scroll";
static char* strSelect = "BD to   select";
static char* strAvailable = "Avail.  apps:";
#endif

/* Global variables */
uint8_t warmResetByte;
CRP* transactionData[MAX_EXCHANGES]; 	// used to log data
uint8_t nTransactions;		            // used to log data
uint8_t lcdAvailable;			        // non-zero if LCD is working
uint8_t nCounter;			            // number of transactions
uint8_t selected;			            // ID of application selected

// Use the LCD as stderr (see main)
FILE lcd_str = FDEV_SETUP_STREAM(LcdPutchar, NULL, _FDEV_SETUP_WRITE);


/**
 * Main program
 */
int main(void)
{
   uint8_t sreg;

   // Init SCD
   InitSCD();

   // Check the hardware
   //TestHardware();

   // Select application if BB is pressed while restarting
   if(GetButtonB() == 0)
   {
       selected = SelectApplication();

       if(selected == APP_ERASE_EEPROM)
       {
               Led2On();
               EraseEEPROM();
               Led2Off();
               wdt_enable(WDTO_15MS);
       }
       else
       {
               sreg = SREG;
               cli();
               eeprom_write_byte((uint8_t*)EEPROM_APPLICATION, selected);
               SREG = sreg;
       }

       // restart SCD so that LCD power is reduced (small trick)
       wdt_enable(WDTO_15MS);
   }
   else
   {
       sreg = SREG;
       cli();
       selected = eeprom_read_byte((uint8_t*)EEPROM_APPLICATION);
       SREG = sreg;

   }

   // run the selected application
   switch(selected)
   {
       case APP_STORE_PIN: 
         StorePIN();
       break;

       case APP_LOG_FORWARD: 
         ForwardData();
       break;

       case APP_FW_MODIFY_PIN: 
         ForwardAndChangePIN();
       break;

       case APP_FILTER_GENERATEAC: 
         FilterGenerateAC();
       break;

       case APP_FILTER_LOG:
         FilterAndLog();
       break;

       case APP_TERMINAL:
         Terminal();
       break;

       case APP_VIRTUAL_SERIAL_PORT:
         VirtualSerial();

       default:
         selected = APP_TERMINAL;
         eeprom_write_byte((uint8_t*)EEPROM_APPLICATION, selected);
         Terminal();
   }

   // if needed disable wdt to avoid restart
   // wdt_disable();

   SwitchLeds();			
}

/**
 * This function shows a menu with the existing applications and allows
 * the user to select one of them. 
 *
 * The different applications are shown on the LCD and the user can
 * use the buttons to scroll between the list and to select the desired
 * application.
 *
 * @return a number representing the selected application. The relation
 * between numbers and applications is done by the define directives
 * (APP_STORE_PIN, etc.). If any error occurs, this method will return
 * 0 (i.e. no application selected).
 */
uint8_t SelectApplication()
{
	volatile uint8_t tmp;
	uint8_t i;

	if(!lcdAvailable) return 0;

	InitLCD();
	fprintf(stderr, "\n");

	while(1){
		fprintf(stderr, "%s\n", strScroll);
		do{
			tmp = GetButton();
		}while((tmp & BUTTON_C) == 0);
		_delay_ms(500);

		fprintf(stderr, "%s\n", strSelect);
		do{
			tmp = GetButton();
		}while((tmp & BUTTON_C) == 0);
		_delay_ms(500);

		fprintf(stderr, "%s\n", strAvailable);
		do{
			tmp = GetButton();
		}while((tmp & BUTTON_C) == 0);	
		_delay_ms(500);

		for(i = 0; i < APPLICATION_COUNT; i++)
		{
			fprintf(stderr, "%s\n", appStrings[i]);
			while(1)
			{
				tmp = GetButton();
				if((tmp & BUTTON_D) != 0) return (i + 1);
				if((tmp & BUTTON_C) != 0) break;			
			}
			_delay_ms(500);
		}
	}

	return 0;
}


/**
 * This method should be called before any other operation. It sets
 * the I/O ports to a correct state and it also recovers any necessary
 * data from EEPROM.
 */
void InitSCD()
{
	// disable any interrupts	
	cli();
	EICRA = 0;
	EICRB = 0;
	EIFR = 0xFF;
	EIMSK = 0;

	// Disable WDT to keep safe operation
	MCUSR = 0;
    wdt_disable();

	// Ports setup
	DDRB = 0x00;

	DDRC = 0x00;
	PORTC = 0x18;		// PC4 with internal pull-up (Terminal I/O)
						// PC3 with internal pull-up (terminal clock)

	DDRD = 0x80;		
	PORTD = 0x83;		// PD7 high (ICC VCC) , PD1 pull-up
						// (ICC switch), PD0 pull-up (terminal reset)

	DDRF &= 0xF0;
	PORTF |= 0x0F; 		// enable pull-up for buttons	

	// change CLK Prescaler value
	clock_prescale_set(clock_div_1); 	

	// light power led
	Led4On();	

#if ICC_PRES_INT_ENABLE
	// enable INT1 on any edge
	EICRA |= _BV(ISC10);
	EICRA &= ~(_BV(ISC11));
	EIMSK |= _BV(INT1);
#endif	

	// Read Warm byte info from EEPROM
	warmResetByte = eeprom_read_byte((uint8_t*)EEPROM_WARM_RESET);

	// Read number of transactions in EEPROM
	nCounter = eeprom_read_byte((uint8_t*)EEPROM_COUNTER);	
	
	// Reset transaction index for log	
	nTransactions = 0;

	// Check LCD status and use as stderr if status OK
	if(CheckLCD())
	{
		stderr = NULL;
		lcdAvailable = 0;
	}
	else	
	{
		stderr = &lcd_str;
		lcdAvailable = 1;
	}

	// disable LCD power
	DDRC &= ~(_BV(PC5));
	PORTC &= ~(_BV(PC5));	
	SetLCDState(0);

	// Disable most modules; they should be re-enabled when needed
	power_adc_disable();
	power_spi_disable();
	power_twi_disable();
	power_usart1_disable();
	//power_usb_disable();

	// Enable interrupts
	sei();	
}


/* Intrerupts */

/**
 * Interrupt routine for INT0. This interrupt can fire when the
 * reset signal from the Terminal goes low (active) and the corresponding
 * interrupt is enabled
 */
ISR(INT0_vect)
{
	uint8_t i;
	uint8_t *stream = NULL, lStream = 0;
	uint16_t addrStream;
	uint8_t addrHi, addrLo;
	uint8_t cmdstr[] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
	uint8_t rspstr[] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
	uint8_t appstr[] = {0xDD, 0xDD, 0xDD, 0xDD, 0xDD};
	uint8_t endstr[] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB};

	// disable wdt
	MCUSR = 0;
    wdt_disable();

	// Update transaction counter in case it was modified
    eeprom_write_byte((uint8_t*)EEPROM_COUNTER, nCounter);

	// Get current transaction log pointer
	addrHi = eeprom_read_byte((uint8_t*)EEPROM_TLOG_POINTER_HI);
	addrLo = eeprom_read_byte((uint8_t*)EEPROM_TLOG_POINTER_LO);
	addrStream = ((addrHi << 8) | addrLo);

	if(addrStream == 0xFFFF) addrStream = EEPROM_TLOG_DATA;

	
	if(nTransactions > 0 && addrStream < EEPROM_MAX_ADDRESS)
	{
		// write selected application ID
		eeprom_write_block(appstr, (void*)addrStream, 5);
		addrStream += 5;
		eeprom_write_byte((uint8_t*)addrStream, selected);
		addrStream += 1;

		// serialize and write transaction data to EEPROM
		for(i = 0; i < nTransactions; i++)
		{
			// Write command to EEPROM
			stream = SerializeCommand(transactionData[i]->cmd, &lStream);
			if(stream != NULL)
			{
				eeprom_write_block(cmdstr, (void*)addrStream, 5);
				addrStream += 5;
				eeprom_write_block(stream, (void*)addrStream, lStream);
				free(stream);
				stream = NULL;
				addrStream += lStream;
				if(addrStream > EEPROM_MAX_ADDRESS) break;
			}

			// Write response to EEPROM
			stream = SerializeResponse(transactionData[i]->response, &lStream);
			if(stream != NULL)
			{
				eeprom_write_block(rspstr, (void*)addrStream, 5);
				addrStream += 5;
				eeprom_write_block(stream, (void*)addrStream, lStream);
				free(stream);
				stream = NULL;
				addrStream += lStream;
				if(addrStream > EEPROM_MAX_ADDRESS) break;
			}

			FreeCRP(transactionData[i]);	
		}

		// write end of log
	    eeprom_write_block(endstr, (void*)addrStream, 5);
		addrStream += 5;		

		// update log address
		addrHi = (uint8_t)((addrStream >> 8) & 0x00FF);
		addrLo = (uint8_t)(addrStream & 0x00FF);
		addrLo += 8;
		addrLo = addrLo & 0xF8; // make the address multiple
								// of 8 (page size)
        eeprom_write_byte((uint8_t*)EEPROM_TLOG_POINTER_HI, addrHi);
        eeprom_write_byte((uint8_t*)EEPROM_TLOG_POINTER_LO, addrLo);
	}

	if(GetTerminalFreq())
	{
		// warm reset
		warmResetByte = eeprom_read_byte((uint8_t*)EEPROM_WARM_RESET);

		if(warmResetByte == WARM_RESET_VALUE)
		{
			// we already had a warm reset so go to initial state
                        eeprom_write_byte((uint8_t*)EEPROM_WARM_RESET, 0);
			while(EECR & _BV(EEPE));			
		}
		else
		{
			// set 0xAA in EEPROM meaning we have a warm reset
                        eeprom_write_byte((uint8_t*)EEPROM_WARM_RESET, WARM_RESET_VALUE);
			while(EECR & _BV(EEPE));			
		}		
	}
	else
	{
		// restart SCD
        eeprom_write_byte((uint8_t*)EEPROM_WARM_RESET, 0);
		while(EECR & _BV(EEPE));		
	}

	// disable INT0	
	EIMSK &= ~(_BV(INT0));

	// re-enable wdt to restart device
	wdt_enable(WDTO_15MS);
}

/**
 * Interrupt routine for INT1. This interrupt can fire when the
 * ICC is inserted or removed and the corresponding interrupt is enabled
 */
ISR(INT1_vect)
{	
	if(bit_is_set(PIND, PD1))
	{			
		Led3On();
	}
	else
	{
		Led3Off();		
		DeactivateICC();
	}

}

/**
 * Interrupt routine for Timer3 Compare Match A overflow. This interrupt
 * can fire when the Timer3 matches the OCR3A value and the corresponding
 * interrupt is enabled
 */
ISR(TIMER3_COMPA_vect, ISR_NAKED)
{	
	reti();	// Do nothing, used just to wake up the CPU
}

/**
 * This function performs a test of the hardware.
 */
void TestHardware()
{
#if LCD_ENABLED
	char* strBA = "Press BA";
	char* strBB = "Press BB";
	char* strBC = "Press BC";
	char* strBD = "Press BD";
	char* strAOK = "All fine!";	
#endif


	Led1On();
	_delay_ms(50);
	Led1Off();
	Led2On();
	_delay_ms(50);
	Led2Off();
	Led3On();
	_delay_ms(50);
	Led3Off();
	Led4On();
	_delay_ms(50);
	Led4Off();

#if LCD_ENABLED
	if(lcdAvailable)
	{
		InitLCD();
		fprintf(stderr, "\n");

		WriteStringLCD(strBA, strlen(strBA));		
		while(bit_is_set(PINF, PF3));

		WriteStringLCD(strBB, strlen(strBB));		
		while(bit_is_set(PINF, PF2));

		WriteStringLCD(strBC, strlen(strBC));		
		while(bit_is_set(PINF, PF1));

		WriteStringLCD(strBD, strlen(strBD));		
		while(bit_is_set(PINF, PF0));

		WriteStringLCD(strAOK, strlen(strAOK));		
	}
#endif	
}

/**
 * This function implements an infinite communication loop between
 * the SCD and my terminal emulator, by continuously replying to
 * the SELECT command for "1PAY.SYS.DDF01"
 *
 * The main role of this function is to test the correct transfer
 * of bytes between the terminal and the SCD
 */
void TestSCDTerminal()
{
	char strLCD[16];
	uint8_t tmpa;

	//start Timer for Terminal
	StartCounterTerminal();
	
	// wait for Terminal CLK and send ATR
	while(ReadCounterTerminal() < 100);
	Led1On();	
	while(GetResetStateTerminal() == 0);
	Led2On();
	LoopTerminalETU(10);
	SendT0ATRTerminal(0, 0x0F);
	Led1Off();	


#if LCD_ENABLED
	if(lcdAvailable)
	{
		InitLCD();
		fprintf(stderr, "\n");
		WriteStringLCD(strATRSent, strlen(strATRSent));		
	}
#endif

	while(1)
	{
		// Get SELECT command for "1PAY.SYS.DDF01"
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[0]);	// CLA = 0x00
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[1]);	// INS = 0xA4
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[2]);	// P1 = 0x04
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[3]);	// P2 = 0x00
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[4]);	// P3 = 0x0E

		strLCD[5] = 0;	

		Led1On();
		Led2Off();		

		// Send INS (procedure byte) back
		LoopTerminalETU(20);
		//SendByteTerminalNoParity(0xA4, 0);		
		SendByteTerminalParity(0xA4, 0);		

		Led1Off();
		Led2On();

		// Get Select command data => "1PAY.SYS.DDF01"
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[0]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[1]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[2]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[3]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[4]);	
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[5]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[6]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[7]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[8]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[9]);	
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[10]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[11]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[12]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[13]);		
		strLCD[14] = 0;	
		
		Led1On();
		Led2Off();

#if LCD_ENABLED
		if(lcdAvailable)
		{
			if(tmpa != 0)
				WriteStringLCD(strError, strlen(strError));
			else
				WriteStringLCD(strLCD, 14);		
		}
#endif

		// Send "6104" (procedure bytes) back		
		SendByteTerminalParity(0x61, 0);		
		LoopTerminalETU(2);		
		SendByteTerminalParity(0x04, 0);		

		Led1Off();
		Led2On();

		// Get GetResponse from Reader
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[0]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[1]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[2]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[3]);		
		tmpa = GetByteTerminalParity(0, (uint8_t*)&strLCD[4]);	
		strLCD[5] = 0;	

		Led1On();
		Led2Off();

		// Send some data back as response to select command
		LoopTerminalETU(20);		
		SendByteTerminalParity(0xC0, 0);		
		LoopTerminalETU(2);
		
		SendByteTerminalParity(0xDE, 0);		
		LoopTerminalETU(2);
		
		SendByteTerminalParity(0xAD, 0);		
		LoopTerminalETU(2);
		
		SendByteTerminalParity(0xBE, 0);		
		LoopTerminalETU(2);
		
		SendByteTerminalParity(0xEF, 0);		
		LoopTerminalETU(2);
		
		SendByteTerminalParity(0x90, 0);		
		LoopTerminalETU(2);
		
		SendByteTerminalParity(0x00, 0);		

		Led1Off();
		Led2On();

#if LCD_ENABLED
		if(lcdAvailable)
			WriteStringLCD(strDataSent, strlen(strDataSent));
#endif
	}
}

/**
 * This function makes a simple test with the ICC
 *
 * It powers the ICC, expects the ATR and sends a select
 * command, waiting also for the answer
 */
void TestSCDICC()
{
	uint8_t inverse, proto, TC1, TA3, TB3;
	uint8_t byte;

	// Power ICC and get ATR
	if(ResetICC(0, &inverse, &proto, &TC1, &TA3, &TB3)) return;
 
	// Send SELECT command (no data yet)
	LoopICCETU(5);
	SendByteICCParity(0x00, inverse);
	LoopICCETU(2);
	SendByteICCParity(0xA4, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x04, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x00, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x0E, inverse);

	// Get INS back
	LoopICCETU(1);
	GetByteICCParity(inverse, &byte);
	if(byte != 0xA4) return;

	// Send "1PAY.SYS.DDF01"
	LoopICCETU(5);
	SendByteICCParity(0x31, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x50, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x41, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x59, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x2E, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x53, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x59, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x53, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x2E, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x44, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x44, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x46, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x30, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x31, inverse);
	
	// Expect 0x61, 0xXX
	LoopICCETU(1);
	GetByteICCParity(inverse, &byte); 
	if(byte != 0x61) return;
	LoopICCETU(1);
	GetByteICCParity(inverse, &byte);

	// Send GET Response
	LoopICCETU(5);
	SendByteICCParity(0x00, inverse);
	LoopICCETU(2);
	SendByteICCParity(0xC0, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x00, inverse);
	LoopICCETU(2);
	SendByteICCParity(0x00, inverse);
	LoopICCETU(2);
	SendByteICCParity(byte, inverse);

	// Data should follow from the ICC...
	Led1On();
#if LCD_ENABLED
	if(lcdAvailable)
	{
		InitLCD();
		fprintf(stderr, "\n");
		WriteStringLCD(strDataSent, strlen(strDataSent));		
	}
#endif
}

/** 
 * This function is used just to switch leds on and off
 */
void SwitchLeds()
{
   while(1)
   {
      _delay_ms(500);
      Led1On();
      Led2Off();
      _delay_ms(500);
      Led1Off();
      Led2On();
   }
}
