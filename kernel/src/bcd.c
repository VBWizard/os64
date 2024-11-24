#include "bcd.h"

uint8_t bcdToDec(uint8_t val)
{
  return ( (val/16*10) + (val%16) );
}

uint8_t decToBcd(uint8_t val)
{
  return ( (val/10*16) + (val%10) );
}
