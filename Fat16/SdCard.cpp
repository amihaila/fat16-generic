/* Arduino FAT16 Library
 * Copyright (C) 2008 by William Greiman
 *
 * This file is part of the Arduino FAT16 Library
 *
 * This Library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with the Arduino Fat16 Library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include <Fat16Config.h>
#include <SdCard.h>
//------------------------------------------------------------------------------
#if 0//////////////////////////////////////////////////////////////////////////////////////////////////
// r1 status values
uint8_t const R1_READY_STATE = 0;
uint8_t const R1_IDLE_STATE  = 1;
// start data token for read or write
uint8_t const DATA_START_BLOCK = 0XFE;
// data response tokens for write block
uint8_t const DATA_RES_MASK        = 0X1F;
uint8_t const DATA_RES_ACCEPTED    = 0X05;
uint8_t const DATA_RES_CRC_ERROR   = 0X0B;
uint8_t const DATA_RES_WRITE_ERROR = 0X0D;
#endif/////////////////////////////////////////////////////////////////////////////////

//==============================================================================
// SdCard member functions
//------------------------------------------------------------------------------
uint8_t SdCard::cardCommand(uint8_t cmd, uint32_t arg) {
  uint8_t r1;

  // select card
  chipSelectLow();

  // wait if busy
  waitForToken(0XFF, SD_WRITE_TIMEOUT);

  // send command
  spiSendByte(cmd | 0x40);

  // send argument
  for (int8_t s = 24; s >= 0; s -= 8) spiSendByte(arg >> s);

  // send CRC - must send valid CRC for CMD0
  spiSendByte(cmd == CMD0 ? 0x95 : 0XFF);

  // wait for not busy
  for (uint8_t retry = 0; (0X80 & (r1 = spiRecByte())) && retry != 0XFF; retry++);
  return r1;
}

//------------------------------------------------------------------------------
// wait for card to go not busy
// return false if timeout
bool SdCard::waitForToken(uint8_t token, uint16_t timeoutMillis) {
  uint16_t t0 = millis();
  while (spiRecByte() != token) {
    if (((uint16_t)millis() - t0) > timeoutMillis) return false;
  }
  return true;
}

//------------------------------------------------------------------------------
uint8_t SdCard::cardAcmd(uint8_t cmd, uint32_t arg) {
  cardCommand(CMD55, 0);
  return cardCommand(cmd, arg);
}
//------------------------------------------------------------------------------
/**
 * Determine the size of a standard SD flash memory card
 * \return The number of 512 byte data blocks in the card
 */
uint32_t SdCard::cardSize(void) {
  uint16_t c_size;
  csd_t csd;
  if (!readReg(CMD9, &csd)) return 0;
  uint8_t read_bl_len = csd.v1.read_bl_len;
  c_size = (csd.v1.c_size_high << 10) | (csd.v1.c_size_mid << 2) | csd.v1.c_size_low;
  uint8_t c_size_mult = (csd.v1.c_size_mult_high << 1) | csd.v1.c_size_mult_low;
  return (uint32_t)(c_size+1) << (c_size_mult + read_bl_len - 7);
}

//------------------------------------------------------------------------------
void SdCard::error(uint8_t code, uint8_t data) {
  errorData = data;
  error(code);
}
//------------------------------------------------------------------------------
void SdCard::error(uint8_t code) {
  errorCode = code;
  chipSelectHigh();
}
//------------------------------------------------------------------------------
/**
 * Initialize a SD flash memory card.
 *
 * \return The value one, true, is returned for success and
 * the value zero, false, is returned for failure.
 *
 */
bool SdCard::begin(void) {

  errorCode = 0;
  uint8_t r;
  // 16-bit init start time allows over a minute
  uint16_t t0 = (uint16_t)millis();

  chipSelectHigh();

  // Enable SPI, Master, clock rate F_CPU/128
  SPCR = (1 << SPE) | (1 << MSTR) | (1 << SPR1) | (1 << SPR0);

  // must supply min of 74 clock cycles with CS high.
  for (uint8_t i = 0; i < 10; i++) spiSendByte(0XFF);
  chipSelectLow();

  // command to go idle in SPI mode
  while ((r = cardCommand(CMD0, 0)) != R1_IDLE_STATE) {
    if (((uint16_t)millis() - t0) > SD_INIT_TIMEOUT) {
      error(SD_ERROR_CMD0, r);
      return false;
    }
  }
#if USE_ACMD41
  // start initialization and wait for completed initialization
  while ((r = cardAcmd(ACMD41, 0)) != R1_READY_STATE) {
    if (((uint16_t)millis() - t0) > SD_INIT_TIMEOUT) {
      error(SD_ERROR_ACMD41, r);
      return false;
    }  
  }
#else  // USE_ACMD41
  // use CMD1 to initialize the card - works with MMC and some SD cards
  while ((r = cardCommand(CMD1, 0)) != R1_READY_STATE) {
    if (((uint16_t)millis() - t0) > SD_INIT_TIMEOUT) {
      error(SD_ERROR_CMD1, r);
      return false;
    }   
  }
#endif  // USE_ACMD41
  chipSelectHigh();
  return true;
}
//------------------------------------------------------------------------------
/**
 * Reads a 512 byte block from a storage device.
 *
 * \param[in] blockNumber Logical block to be read.
 * \param[out] dst Pointer to the location that will receive the data.
 * \return The value one, true, is returned for success and
 * the value zero, false, is returned for failure.
 */
bool SdCard::readBlock(uint32_t blockNumber, uint8_t* dst) {
  if (cardCommand(CMD17, blockNumber << 9)) {
    error(SD_ERROR_CMD17);
    return false;
  }
  return readTransfer(dst, 512);
}
//------------------------------------------------------------------------------
bool SdCard::readReg(uint8_t cmd, void* buf) {
  uint8_t* dst = reinterpret_cast<uint8_t*>(buf);
  if (cardCommand(cmd, 0)) {
    chipSelectHigh();
    return false;
  }
  return readTransfer(dst, 16);
}
//------------------------------------------------------------------------------
bool SdCard::readTransfer(uint8_t* dst, uint16_t count) {
  // wait for start of data
  if (!waitForToken(DATA_START_BLOCK, SD_READ_TIMEOUT)) {
    error(SD_ERROR_READ_TIMEOUT);
  }
  // start first spi transfer
  SPDR = 0XFF;
  for (uint16_t i = 0; i < count; i++) {
    while (!(SPSR & (1 << SPIF)));
    dst[i] = SPDR;
    SPDR = 0XFF;
  }
  // wait for first CRC byte
  while (!(SPSR & (1 << SPIF)));
  spiRecByte();  // second CRC byte
  chipSelectHigh();
  return true;
}
//------------------------------------------------------------------------------
/**
 * Writes a 512 byte block to a storage device.
 *
 * \param[in] blockNumber Logical block to be written.
 * \param[in] src Pointer to the location of the data to be written.
 * \return The value one, true, is returned for success and
 * the value zero, false, is returned for failure.
 */
bool SdCard::writeBlock(uint32_t blockNumber, const uint8_t* src) {
  uint32_t address = blockNumber << 9;

  if (cardCommand(CMD24, address)) {
    error(SD_ERROR_CMD24);
    return false;
  }
  // optimize write loop
  SPDR = DATA_START_BLOCK;
  for (uint16_t i = 0; i < 512; i++) {
    while (!(SPSR & (1 << SPIF)));
    SPDR = src[i];
  }
  while (!(SPSR & (1 << SPIF)));  // wait for last data byte
  spiSendByte(0xFF);  // dummy crc
  spiSendByte(0xFF);  // dummy crc

  // get write response
  uint8_t r1 = spiRecByte();
  if ((r1 & DATA_RES_MASK) != DATA_RES_ACCEPTED) {
    error(SD_ERROR_WRITE_RESPONSE, r1);
    return false;
  }
  // wait for card to complete write programming
  if (!waitForToken(0XFF, SD_WRITE_TIMEOUT)) {
      error(SD_ERROR_WRITE_TIMEOUT);
  }
  chipSelectHigh();
  return true;
}