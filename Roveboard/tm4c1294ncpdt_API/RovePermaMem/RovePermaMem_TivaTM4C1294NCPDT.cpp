/*
 * RovePermaMem_TivaTM4C1294NCPDT.cpp
 *
 *  Created on: Oct 21, 2017
 *      Author: drue
 */

#include "RovePermaMem_TivaTM4C1294NCPDT.h"
#include "RovePermaMem_Private.h"
#include <string.h>
#include "../tivaware/driverlib/eeprom.h"
#include "supportingUtilities/Debug.h"

//blocks 0 to 5 are reserved for roveboard stuff. Block 0 especially, being reserved for this file.
//Users can input 0 to 89, but in reality it gets shifted up to 6 to 95
static const uint8_t BlockIndex_ReservedBlocks = 6;
static const uint8_t BlockIndex_ControlBlock = 0;

//variables from rovePermaMem_Private. Noted in comment for viewing
/*const uint8_t BlockReferenceTableSize = 3;
const word_t DummyMaskInLastWordOfTable = 0b11111111111111111111111111000000;
const uint32_t ControlBlockPassword = 0x0000EFFE;
const uint8_t WordLengthInBytes = 4;
static const uint8_t ControlBlock_WordOffset_blockFresh = 1 * WordLengthInBytes; //actually goes from 1 to 3
static const uint8_t ControlBlock_WordLength_blockFresh = 3 * WordLengthInBytes;*/

static const uint8_t BytesPerBlock = 64;
static const uint8_t TotalBlocks = 96 - BlockIndex_ReservedBlocks;
static const uint8_t bitsPerWord = 32;

static word_t blockCountToBitbandWord(uint16_t x);
static bool isBlockFreshOrUsed(uint16_t blockReference, bool whichToGet);
static uint16_t getTotalUnusedOrFreshBlocks(bool whichToGet);
static uint16_t inputBlockIndexToMemIndex(uint16_t inputBlockIndex);
static void lockEeprom();
static void unlockEeprom();
static void udpateUseTable(uint16_t blockReference, bool setUse);
static void udpateFreshTable(uint16_t blockReference);

static const bool getFresh = true;
static const bool getUsed = false;

RovePermaMem_Error rovePermaMem_WriteBlockByte(uint16_t blockReference, uint8_t byteReference, uint8_t password, uint8_t valueToWrite)
{
  if(blockReference >= TotalBlocks || byteReference >= BytesPerBlock)
  {
    return RovePermaMem_InputOutOfBounds;
  }
  else
  {
    bool b;
    rovePermaMem_isBlockUsed(blockReference, &b);
    if(!b)
    {
      return RovePermaMem_BlockNotAllocated;
    }
  }

  uint8_t byteBuff[BytesPerBlock];
  RovePermaMem_Error errVal;

  errVal = rovePermaMem_ReadBlock(blockReference, password, byteBuff);
  if(errVal != RovePermaMem_Success)
  {
    return errVal;
  }

  byteBuff[byteReference] = valueToWrite;

  return rovePermaMem_WriteBlock(blockReference, password, byteBuff);
}

RovePermaMem_Error rovePermaMem_WriteBlock(uint16_t blockReference, uint8_t password, uint8_t bytes[])
{
  if(blockReference >= TotalBlocks || bytes == 0)
  {
    return RovePermaMem_InputOutOfBounds;
  }
  else
  {
    bool b;
    rovePermaMem_isBlockUsed(blockReference, &b);
    if(!b)
    {
      return RovePermaMem_BlockNotAllocated;
    }
  }

  uint16_t realBlockReference;
  const bool Shitslocked = 0;
  bool isShitStillLocked;

  //update the user's block index to make it fit with the actual EEPROM block mapping
  realBlockReference = inputBlockIndexToMemIndex(blockReference);

  unlockEeprom();
  isShitStillLocked = EEPROMBlockUnlock(realBlockReference, (uint32_t*)&password, 1);
  if(isShitStillLocked == Shitslocked)
  {
    return RovePermaMem_ImproperPassword;
  }

  EEPROMProgram((uint32_t*)bytes, EEPROMAddrFromBlock(realBlockReference), BytesPerBlock);

  EEPROMBlockLock(realBlockReference);
  lockEeprom();

  return RovePermaMem_Success;
}

RovePermaMem_Error rovePermaMem_ReadBlockByte(uint16_t blockReference, uint8_t byteReference, uint8_t password, uint8_t *readBuffer)
{
  if(blockReference >= TotalBlocks || byteReference >= BytesPerBlock || readBuffer == 0)
  {
    return RovePermaMem_InputOutOfBounds;
  }
  else
  {
    bool b;
    rovePermaMem_isBlockUsed(blockReference, &b);
    if(!b)
    {
      return RovePermaMem_BlockNotAllocated;
    }
  }

  uint8_t byteBuff[BytesPerBlock];
  RovePermaMem_Error errVal;

  errVal = rovePermaMem_ReadBlock(blockReference, password, byteBuff);
  if(errVal != RovePermaMem_Success)
  {
    return errVal;
  }

  *readBuffer = byteBuff[byteReference];

  return RovePermaMem_Success;
}

RovePermaMem_Error rovePermaMem_ReadBlock(uint16_t blockReference, uint8_t password, uint8_t byteBuffer[])
{
  if(blockReference >= TotalBlocks || byteBuffer == 0)
  {
    return RovePermaMem_InputOutOfBounds;
  }
  else
  {
    bool b;
    rovePermaMem_isBlockUsed(blockReference, &b);
    if(!b)
    {
     return RovePermaMem_BlockNotAllocated;
    }
  }

  uint16_t realBlockReference;
  const bool Shitslocked = 0;
  bool isShitStillLocked;

  //update the user's block index to make it fit with the actual EEPROM block mapping
  realBlockReference = inputBlockIndexToMemIndex(blockReference);

  unlockEeprom();
  isShitStillLocked = EEPROMBlockUnlock(realBlockReference, (uint32_t*)&password, 1);
  if(isShitStillLocked == Shitslocked)
  {
   return RovePermaMem_ImproperPassword;
  }

  EEPROMRead(((uint32_t*)byteBuffer), EEPROMAddrFromBlock(realBlockReference), BytesPerBlock);

  EEPROMBlockLock(realBlockReference);
  lockEeprom();

  return RovePermaMem_Success;
}

RovePermaMem_Error rovePermaMem_useBlock(uint16_t blockReference, uint16_t passwordToUse)
{
  if(blockReference >= TotalBlocks)
  {
    return RovePermaMem_InputOutOfBounds;
  }
  else
  {
    bool b;
    rovePermaMem_isBlockUsed(blockReference, &b);
    if(b)
    {
      return RovePermaMem_AlreadyUsed;
    }
  }

  //inputs having been checked, reserve the block requested by unlocking the EEPROM module
  //and setting the hardware to use the user's password whenever that block is used
  uint32_t writeVal;
  uint16_t realBlockReference;

  //update the user's block index to make it fit with the actual EEPROM block mapping
  realBlockReference = inputBlockIndexToMemIndex(blockReference);

  //begin writing things to eeprom
  unlockEeprom();

  writeVal = passwordToUse;
  EEPROMBlockPasswordSet(EEPROMAddrFromBlock(realBlockReference), &writeVal, 1);
  EEPROMBlockLock(realBlockReference);

  //update tables. Eeprom remains unlocked so the functions can update eeprom if need be
  udpateUseTable(blockReference, true);
  udpateFreshTable(blockReference);

  lockEeprom();

  return RovePermaMem_Success;
}

bool rovePermaMem_getFirstAvailableBlock(bool onlyGetFreshBlocks, uint16_t startingBlock, uint16_t *ret_blockReference)
{
  if(startingBlock >= TotalBlocks)
  {
    return false;
  }

  const uint16_t initValue = 0xFFFF; //way beyond what the size can actually get to, so serves as a good 'hasn't been set yet' value
  int i, j;
  int i_start;
  int j_start;

  word_t bitBand;
  *ret_blockReference = initValue;

  word_t blockUnusedTable[BlockReferenceTableSize];

  //blockUsedTable has a 1 for every used and 0 for unused, so invert it to get an unused table.
  //while we're at it, let's save ourselves another for loop and initialize j_start
  for(i = 0; i < BlockReferenceTableSize; i++)
  {
    blockUnusedTable[i] = ~(blockUsedTable[i]);
  }

  //blockUsedTable lists a 0 for all dummy values which get turned into 1 for the unused table.
  //set them back to 0 to make it look like they're used so the search algorithm won't select them
  blockUnusedTable[BlockReferenceTableSize - 1] &= DummyMaskInLastWordOfTable;

  //figure out where in the search algorithm we want to start at.
  if(startingBlock < bitsPerWord)
  {
    i_start = 0;
    j_start = startingBlock;
  }
  else if(startingBlock >= bitsPerWord * 2)
  {
    i_start = 2;
    j_start = startingBlock - bitsPerWord * i_start;
  }
  else
  {
    i_start = 1;
    j_start = startingBlock - bitsPerWord * i_start;
  }

  //90 total available blocks, but kept in an array of 32 bits. So, search through the array by checking
  // each of the 32 bits in each of its indexes.
  for(i = i_start; i < BlockReferenceTableSize; i++)
  {
    for(j = j_start; j < bitsPerWord; j++)
    {
      bitBand = blockCountToBitbandWord(j);
      if(bitBand & blockUnusedTable[i])
      {
        if(!onlyGetFreshBlocks || (bitBand & blockFreshTable[i]))
        {
          *ret_blockReference = j + i * bitsPerWord;
          break;
        }
      }
    }

    //check to see if we found anything yet. If so, go ahead and exit loop
    if(*ret_blockReference != initValue)
    {
      break;
    }

    //j_start needs to be 0 for every loop except for the starting point so that it knows where to start
    //but afterwards won't start skipping numbers for all iterations of i after
    j_start = 0;
  }

  if(*ret_blockReference != initValue)
  {
    return true;
  }
  else
  {
    return false;
  }
}

uint16_t rovePermaMem_getTotalUsedBlocks()
{
  return getTotalUnusedOrFreshBlocks(getUsed);
}

uint16_t rovePermaMem_getTotalUnusedBlocks()
{
  //Incidentally, it's easiest to only do the actual searching when looking for Used or Fresh blocks and do this
  //kind of return when looking for unused or spoiled blocks, as the search algorithm for used or fresh blocks
  //can just skip over the dummy values in the block arrays whereas we'd need to put in extra logic to account for
  //said dummy values if we were doing array searching to figure out unused or spoiled blocks instead
  return TotalBlocks - rovePermaMem_getTotalUsedBlocks();
}

uint16_t rovePermaMem_getTotalFreshBlocks()
{
  return getTotalUnusedOrFreshBlocks(getFresh);
}

uint16_t rovePermaMem_getBytesPerBlock()
{
  return BytesPerBlock;
}
uint16_t rovePermaMem_getTotalBlocks()
{
  return TotalBlocks;
}

RovePermaMem_Error rovePermaMem_isBlockUsed(uint16_t blockReference, bool *retVal)
{
  if(blockReference >= TotalBlocks)
  {
    return RovePermaMem_InputOutOfBounds;
  }

  *retVal = isBlockFreshOrUsed(blockReference, getUsed);

  return RovePermaMem_Success;
}

RovePermaMem_Error rovePermaMem_isBlockFresh(uint16_t blockReference, bool *retVal)
{
  if(blockReference >= TotalBlocks)
  {
    return RovePermaMem_InputOutOfBounds;
  }

  *retVal = isBlockFreshOrUsed(blockReference, getFresh);

  return RovePermaMem_Success;
}

static bool isBlockFreshOrUsed(uint16_t blockReference, bool whichToGet)
{
  word_t blockWord;
  word_t blockReferenceBitband;
  word_t *blockTable;

  if(whichToGet == getFresh)
  {
    blockTable = blockFreshTable;
  }
  else
  {
    blockTable = blockUsedTable;
  }

  //global table is expressed in words
  if(blockReference < bitsPerWord)
  {
    blockWord = blockTable[0];
  }
  else if(blockReference >= bitsPerWord * 2)
  {
    blockWord = blockTable[2];
    blockReference -= bitsPerWord * 2;
  }
  else
  {
    blockWord = blockTable[1];
    blockReference -= bitsPerWord;
  }

  blockReferenceBitband = blockCountToBitbandWord(blockReference);

  return blockReferenceBitband & blockWord;
}

static uint16_t getTotalUnusedOrFreshBlocks(bool whichToGet)
{
  int i, j;
  int count = 0;
  word_t bitBand;
  word_t *tableReference;

  if(whichToGet == getFresh)
  {
    tableReference = blockFreshTable;
  }
  else
  {
    tableReference = blockUsedTable;
  }

  //90 total available blocks, but kept in an array of 32 bits. So, search through the array by checking
  // each of the 32 bits in each of its indexes.
  for(i = 0; i < BlockReferenceTableSize; i++)
  {
    for(j = 0; j < bitsPerWord; j++)
    {
      bitBand = blockCountToBitbandWord(j);
      if(bitBand & tableReference[i])
      {
        //technically everything from 90 to 96 are dummy values, but they don't affect the count because rovePermaMem_init sets them to 0
        count++;
      }
    }
  }

  return count;
}

static word_t blockCountToBitbandWord(uint16_t x)
{
  #ifndef ROVEDEBUG_NO_DEBUG
  if(x >= bitsPerWord)
  {
    debugFault("You idiot drue, you didn't constrain your inputs properly");
  }
  #endif

  //our tables go from left to right for counting
  return 0b10000000000000000000000000000000 >> x;
}

static uint16_t inputBlockIndexToMemIndex(uint16_t inputBlockIndex)
{
  return inputBlockIndex += BlockIndex_ReservedBlocks;
}

static void unlockEeprom()
{
  uint32_t writeVal;

  writeVal = ControlBlockPassword;
  EEPROMBlockUnlock(BlockIndex_ControlBlock, &writeVal, 1);
}
static void lockEeprom()
{
  EEPROMBlockLock(BlockIndex_ControlBlock);
}

static void udpateUseTable(uint16_t blockReference, bool setUse)
{
  uint8_t tableIndex;
  word_t bitband;
  if(blockReference < bitsPerWord)
  {
    tableIndex = 0;
  }
  else if(blockReference >= bitsPerWord * 2)
  {
    tableIndex = 2;
    blockReference -= bitsPerWord * 2;
  }
  else
  {
    tableIndex = 1;
    blockReference -= bitsPerWord;
  }

  bitband = blockCountToBitbandWord(blockReference);

  if(!setUse)
  {
    blockUsedTable[tableIndex] &= ~bitband;
  }
  else
  {
    blockUsedTable[tableIndex] |= bitband;
  }
}

//precall: must have eeprom unlocked
static void udpateFreshTable(uint16_t blockReference)
{
  uint8_t tableIndex;
  word_t bitband;
  if(blockReference < bitsPerWord)
  {
    tableIndex = 0;
  }
  else if(blockReference >= bitsPerWord * 2)
  {
    tableIndex = 2;
    blockReference -= bitsPerWord * 2;
  }
  else
  {
    tableIndex = 1;
    blockReference -= bitsPerWord;
  }

  bitband = blockCountToBitbandWord(blockReference);
  blockFreshTable[tableIndex] &= ~bitband;
  EEPROMProgram(blockFreshTable, EEPROMAddrFromBlock(BlockIndex_ControlBlock) + ControlBlock_WordOffset_blockFresh, ControlBlock_WordLength_blockFresh);
}

