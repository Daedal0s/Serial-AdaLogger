/*
 Serial data recorder, by Justin Eskridge 2017-8-8.
  Starting with example code from Tom Igoe and Adafruit.

 Hardware
  230kbaud 8N1 serial data source
  4GB SD-card
  Adafruit SAMD21-based Feather Adalogger

 I tried to make this work using DMA to automatically transfer data from UART RX buffer.
  But I ran into difficulty in setting up the DMA controller to make a ring buffer in RAM.

 This interrupt-based version works fine, so long as the SERIAL_BUFFER_SIZE is sufficiently large
  to store all characters while the CPU is busy with the SD card.

 Your compiler will probably throw a warning about "SERIAL_BUFFER_SIZE" getting redefined in the SAMD library code.
   The only way I found to make it work is by modifying the #DEFINE statement in the library source to match the one here.

 Suggestions and modifications welcome.
*/

// -------------------------------------------
// Dependencies
// -------------------------------------------
#include <SPI.h>
#include <SD.h>
#include <Arduino.h>   // required before wiring_private.h
//#include <Adafruit_ZeroDMA.h>
//#include <Adafruit_ASFcore.h>
//#include "utility/dmac.h"
//#include "utility/dma.h"
#include "wiring_private.h" // pinPeripheral() function

// -------------------------------------------
// Parameters
// -------------------------------------------
/* 
 SD space available is approx 4x10^9 bytes
 Each line of data from source is 45 bytes
 At high speed, a new line is generated every 2ms
 At 230400 baud, each byte takes 43.4us to transmit
 Transmit time for each line is thus 43.4us x 45 = 1.953ms (very little extra time)
 Data fill rate is approx 45*1000/2 = 22,500 bytes / second = 1,350,000 bytes / minute = 1.287MB / minute
 Recording time in minutes at full speed on a 4GB card is T = (4*10^9)/(1.35*10^6) = 2963 minutes ~= 49 hours max

 Let's go with 1300 files, Dat000.log to Dat1299.log
 For now, just hard-code the filesize to about 120s (2,700,000 bytes) each for a max of about 3,510,000,000 bytes of data.
 Should be plenty of headroom to avoid running out of space

 ** TODO : add code that measures free space and acts accordingly **
*/
#define LOG_BYTE_TRIGGER      11250
#define LOG_TIME_TRIGGER       3000
#define LOG_FILE_SIZE_TRIGGER  2700000

#define LOG_FILE_NUMBER_MAX      1299
#define LOG_FILE_PREFIX "Dat"
#define LOG_FILE_SUFFIX ".log"
#define LOG_FILE(x) (String(LOG_FILE_PREFIX + Str_Zeros(x) + String(x) + LOG_FILE_SUFFIX))

#define SERIAL_RX_BUFFER_SIZE 8192//4096
#define SERIAL_BUFFER_SIZE  8192//4096

// -------------------------------------------
// Debug LEDs
// -------------------------------------------
#define DBUG_RED_LED_PIN 13
#define DBUG_GRN_LED_PIN 8

#define DBUG_GRN_LED_SET(x)   (digitalWrite(DBUG_GRN_LED_PIN, x))
#define DBUG_RED_LED_SET(x)   (digitalWrite(DBUG_RED_LED_PIN, x))

// -------------------------------------------
// Extra debug pins for scope
// -------------------------------------------
#define DBUG_VIO_PIN 9
#define DBUG_GRN_PIN 6
#define DBUG_BLU_PIN 12

#define DBUG_VIO_SET(x)   (digitalWrite(DBUG_VIO_PIN, x))
#define DBUG_GRN_SET(x)   (digitalWrite(DBUG_GRN_PIN, x))
#define DBUG_BLU_SET(x)   (digitalWrite(DBUG_BLU_PIN, x))

// -------------------------------------------------------
// Special define to fix virtual serial port issue
// -------------------------------------------------------
#if defined(ARDUINO_SAMD_ZERO) && defined(SERIAL_PORT_USBVIRTUAL)
  // Required for Serial on Zero based boards
  #define Serial SERIAL_PORT_USBVIRTUAL
#endif

// -------------------------------------------------------
// For DMA transfers
// -------------------------------------------------------
/*Adafruit_ZeroDMA myDMA;

// Called each time a DMA transfer finishes
void dma_callback(struct dma_resource* const resource) {
}*/

// -------------------------------------------------------
// SD Card config
// -------------------------------------------------------
const int chipSelect = 4;

// -------------------------------------------------------
// Create a new serial port on pins 10&11
// -------------------------------------------------------
Uart Serial2 (&sercom1, 11, 10, SERCOM_RX_PAD_0, UART_TX_PAD_2);

// -------------------------------------------------------
// Variables for the logger
// -------------------------------------------------------
uint32_t Log_Time = 0;
uint32_t Log_Byte_Count = 0;
uint32_t Log_File_Byte_Count = 0;
uint32_t Log_File_Number = 0;
File dataFile;

String Str_Zeros(int x) {
  if (x > 999) return "";
  if (x > 99) return "0";
  if (x > 9) return "00";
  return "000";
}

// -------------------------------------------------------
// -------------------------------------------------------
// SETUP
// -------------------------------------------------------
// -------------------------------------------------------
void setup() {
  // Set debug pins as outputs
  pinMode(DBUG_RED_LED_PIN, OUTPUT);
  pinMode(DBUG_GRN_LED_PIN, OUTPUT);
  pinMode(DBUG_VIO_PIN, OUTPUT);
  pinMode(DBUG_GRN_PIN, OUTPUT);
  pinMode(DBUG_BLU_PIN, OUTPUT);
  
  // Set debug pin states
  DBUG_GRN_LED_SET(HIGH);
  DBUG_VIO_SET(HIGH);
  DBUG_GRN_SET(HIGH);
  DBUG_BLU_SET(HIGH);
  DBUG_RED_LED_SET(HIGH);

  // See if the card is present and can be initialized:
  if (!SD.begin(chipSelect)) {
    // Flash a fail signal on bloth LEDs
    while(1) {
      DBUG_GRN_LED_SET(HIGH);
      DBUG_RED_LED_SET(HIGH);
      delay(100);
      DBUG_GRN_LED_SET(LOW);
      DBUG_RED_LED_SET(LOW);
      delay(300);
      DBUG_GRN_LED_SET(HIGH);
      DBUG_RED_LED_SET(HIGH);
      delay(100);
      DBUG_GRN_LED_SET(LOW);
      DBUG_RED_LED_SET(LOW);
      delay(300);
      DBUG_GRN_LED_SET(HIGH);
      DBUG_RED_LED_SET(HIGH);
      delay(100);
      DBUG_GRN_LED_SET(LOW);
      DBUG_RED_LED_SET(LOW);
      delay(1100);
    }
  }
  // Find the next available file name
  Log_File_Number = 0;
  while ((Log_File_Number < LOG_FILE_NUMBER_MAX) && (SD.exists(LOG_FILE(Log_File_Number)))) {
    Log_File_Number++;
  }
  // If we reached the end of the files without finding an empty, just kill file 000 and start over
  if (Log_File_Number > LOG_FILE_NUMBER_MAX) {
    Log_File_Number = 0;
    SD.remove(LOG_FILE(0));
  }

  // Delete the next file in line if present
  if (Log_File_Number == LOG_FILE_NUMBER_MAX) {
    SD.remove(LOG_FILE(0));
  } else {
    SD.remove(LOG_FILE(Log_File_Number+1));
  }

  // open the file. note that only one file can be open at a time,
  // so you have to close this one before opening another.
  dataFile = SD.open(LOG_FILE(Log_File_Number), FILE_WRITE);

  // Setup the serial port that will receive the data for logging
  Serial2.begin(230400);
  
  // Assign pins 10 & 11 SERCOM functionality
  pinPeripheral(10, PIO_SERCOM);
  pinPeripheral(11, PIO_SERCOM);


/*
// -----------------------------------
// Configure DMA for SERCOM1 (our 'SPI1' port on 11/12/13)
// -----------------------------------
  myDMA.configure_peripheraltrigger(SERCOM1_DMAC_ID_RX);  // RX on sercom1 triggers DMA transfer
  myDMA.configure_triggeraction(DMA_TRIGGER_ACTON_BEAT);
  myDMA.allocate();
  myDMA.add_descriptor();
  //myDMA.register_callback(dma_callback);
  //myDMA.enable_callback();


  // Set up DMA transfer using the newly-filled buffer as source...
  myDMA.setup_transfer_descriptor(
    (void *)(&SERCOM1->SPI.DATA.reg), // Source address
    fillBuf,                          // Dest address
    UART_BUFFER_SIZE,                 // Data count
    DMA_BEAT_SIZE_BYTE,               // Bytes/halfwords/words
    false,                            // Increment source address
    true);                            // Don't increment dest

  myDMA.start_transfer_job();
// -----------------------------------
// -----------------------------------*/
}

// -------------------------------------------------------------------------
// MAIN LOOP
// -------------------------------------------------------------------------
void loop() {
  // If there is data available in serial port buffer
  while(Serial2.available()) {
    // Transfer one byte from serial receive buffer to SD file buffer
    dataFile.write(Serial2.read());
    // Increment the byte counters
    Log_File_Byte_Count++;
    Log_Byte_Count++;
    // Dump to disk after LOG_BYTE_TRIGGER bytes or LOG_TIME_TRIGGER milliseconds
    if ((Log_Byte_Count >= LOG_BYTE_TRIGGER) || (millis() >= Log_Time)) {
      DBUG_RED_LED_SET(HIGH);
      // Reset the byte and time triggers
      Log_Byte_Count = 0;
      Log_Time = millis() + LOG_TIME_TRIGGER;
      // Should we close this file or just flush it to disk?
      // How many bytes have we written now?
      if (Log_File_Byte_Count < LOG_FILE_SIZE_TRIGGER) {
        // Just flush the data to disk, don't switch files yet
        dataFile.flush();
      } else {
        // Reset the file bytes count
        Log_File_Byte_Count = 0;
        // Save and close this file
        dataFile.close();
        // Increment the log file number
        if (++Log_File_Number > LOG_FILE_NUMBER_MAX) {
          Log_File_Number = 0;
        }
        // Delete the next file in line if it exists
        if (Log_File_Number == LOG_FILE_NUMBER_MAX) {
          SD.remove(LOG_FILE(0));
        } else {
          SD.remove(LOG_FILE(Log_File_Number+1));
        }
        // Create and open the new file for writing
        dataFile = SD.open(LOG_FILE(Log_File_Number), FILE_WRITE);
      }
      DBUG_RED_LED_SET(LOW);
    }
  }
  DBUG_BLU_SET(LOW);
}

void SERCOM1_Handler() {
  DBUG_BLU_SET(HIGH);
  DBUG_GRN_LED_SET(HIGH);
  Serial2.IrqHandler();
  DBUG_GRN_LED_SET(LOW);
}

