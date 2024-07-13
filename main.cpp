#include "lib/portaudio/include/portaudio.h"
#include </opt/homebrew/Cellar/fftw/3.3.10_1/include/fftw3.h>
#include </usr/local/include/matplot/matplot.h>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <time.h>
#include <unistd.h>

// Define parameters
// const int SAMPLE_RATE = 44100;
const int SAMPLE_RATE = 22050;
const int FRAMES_PER_BUFFER = 128;
// const int FRAMES_PER_BUFFER = 256;
const double TICK_THRESHOLD = 0.03; // Adjust as needed
// const double TICK_THRESHOLD = 0.0009; // Adjust as needed
const int NUM_SECONDS = 5;

// Global variables
int tickCount = 0;
int j;
// Callback function for processing audioCallback
using namespace matplot;

// std::vector<double> x = linspace(0, 2 * pi);
std::vector<double> sample;

static int audioCallback(const void *inputBuffer, void *outputBuffer,
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo *timeInfo,
                         PaStreamCallbackFlags statusFlags, void *userData) {
  float *inBuffer = (float *)inputBuffer;

  // Simple tick detection algorithm

  // for (j = 0; j < (NUM_SECONDS * SAMPLE_RATE) / FRAMES_PER_BUFFER; ++j) {
  for (unsigned int i = 0; i < framesPerBuffer; i++) {
    // std::cout << "inside for loop comparing against tick threshold // "
    //           << inBuffer[i] << std::endl;
    if (std::abs(inBuffer[i]) > TICK_THRESHOLD) {
      tickCount++;
      // std::cout << "Tick detected! Count: " << tickCount << " with value "
      //           << inBuffer[i] << std::endl;
      //     p[0]->marker(matplot::line_spec::marker_style::asterisk);
    }
    sample.push_back(inBuffer[i]);
  }
  //}

  // show();
  //  std::cout << sample.back() << std::endl;
  return paContinue;
}

int main() {

  // Initialize PortAudio
  Pa_Initialize();

  // Set up the audio stream
  PaStream *stream;
  Pa_OpenDefaultStream(&stream, 1, 0, paFloat32, SAMPLE_RATE, FRAMES_PER_BUFFER,
                       audioCallback, nullptr);
  Pa_StartStream(stream);

  std::cout << "Listening for ticks. Press Enter to stop" << std::endl;

  int count = 1;

  clock_t timer_counter;

  clock_t this_time;
  clock_t last_time = clock();

  time_t rawtime;

  time(&rawtime);
  printf("The current local time is: %s", ctime(&rawtime));

  printf("Gran = %ld\n", CLOCKS_PER_SEC);
  while (true) {
    this_time = clock();

    timer_counter = (this_time - last_time);
    // printf("Debug time = %f\n ", ((float)timer_counter / CLOCKS_PER_SEC));

    if (((float)timer_counter / CLOCKS_PER_SEC) >= (2)) {
      time(&rawtime);
      printf("The current local time is: %s", ctime(&rawtime));
      break;
    }
  }
  sample.erase(sample.begin(), sample.begin() + 1000);
  plot(sample);
  // std::cout << sample.size() << std::endl;
  std::cin.get(); // Wait for user input
  //  Stop and close the stream
  Pa_StopStream(stream);
  Pa_CloseStream(stream);
  Pa_Terminate();

  return 0;
}
