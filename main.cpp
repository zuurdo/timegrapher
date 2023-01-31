#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <iostream>

#include "lib/portaudio/include/portaudio.h"

int readWatchInput(void *inputBuffer,
                   void *outputBuffer,
                   unsigned long framesPerBuffer,
                   const PaStreamCallbackTimeInfo* outTime,
                   void *userData)
{

}

int main()
{
  printf("Ey yo");
}