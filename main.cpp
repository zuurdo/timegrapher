#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <iostream>

#include "lib/portaudio/include/portaudio.h"


/*
 * Define constants to record
 * */
/* #define SAMPLE_RATE  (17932) // Test failure to open with this value. */
#define PRINTF_S_FORMAT "%.8f"
#define SAMPLE_RATE  (44100)
//#define SAMPLE_RATE  (30100)
#define FRAMES_PER_BUFFER (64)
#define NUM_SECONDS     (5)
#define NUM_CHANNELS    (1)
/* #define DITHER_FLAG     (paDitherOff) */
#define DITHER_FLAG     (0) /**/
/** Set to 1 if you want to capture the recording to a file. */
#define WRITE_TO_FILE   (0)



/* Select sample format. */
#if 1
#define PA_SAMPLE_TYPE  paFloat32
typedef float SAMPLE;
#define SAMPLE_SILENCE  (0)
#define PRINTF_S_FORMAT "%.8f"
#elif 1
#define PA_SAMPLE_TYPE  paInt16
typedef short SAMPLE;
#define SAMPLE_SILENCE  (0)
#define PRINTF_S_FORMAT "%d"
#elif 0
#define PA_SAMPLE_TYPE  paInt8
typedef char SAMPLE;
#define SAMPLE_SILENCE  (0)
#define PRINTF_S_FORMAT "%d"
#else
#define PA_SAMPLE_TYPE  paUInt8
typedef unsigned char SAMPLE;
#define SAMPLE_SILENCE  (128)
#define PRINTF_S_FORMAT "%d"
#endif

#define OUTPUT_FORMAT  paFloat32
typedef float OUTPUT_SAMPLE;



double gInOutScaler = 1.0;
#define CONVERT_IN_TO_OUT(in)  ((OUTPUT_SAMPLE) ((in) * gInOutScaler))

typedef struct {
    int frameIndex;  /* Index into sample array. */
    int maxFrameIndex;
    SAMPLE *recordedSamples;
}
        paTestData;


/*******************************************************************/
static void PrintSupportedStandardSampleRates(
        const PaStreamParameters *inputParameters,
        const PaStreamParameters *outputParameters) {
  static double standardSampleRates[] = {
          8000.0, 9600.0, 11025.0, 12000.0, 16000.0, 22050.0, 24000.0, 32000.0,
          44100.0, 48000.0, 88200.0, 96000.0, 192000.0, -1 /* negative terminated  list */
  };
  int i, printCount;
  PaError err;

  printCount = 0;
  for (i = 0; standardSampleRates[i] > 0; i++) {
    err = Pa_IsFormatSupported(inputParameters, outputParameters, standardSampleRates[i]);
    if (err == paFormatIsSupported) {
      if (printCount == 0) {
        printf("\t%8.2f", standardSampleRates[i]);
        printCount = 1;
      } else if (printCount == 4) {
        printf(",\n\t%8.2f", standardSampleRates[i]);
        printCount = 1;
      } else {
        printf(", %8.2f", standardSampleRates[i]);
        ++printCount;
      }
    }
  }
  if (!printCount)
    printf("None\n");
  else
    printf("\n");
}

/*******************************************************************/

/* This routine will be called by the PortAudio engine when audio is needed.
** It may be called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int recordCallback(const void *inputBuffer, void *outputBuffer,
                          unsigned long framesPerBuffer,
                          const PaStreamCallbackTimeInfo *timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void *userData) {
  paTestData *data = (paTestData *) userData;
  const SAMPLE *rptr = (const SAMPLE *) inputBuffer;
  SAMPLE *wptr = &data->recordedSamples[data->frameIndex * NUM_CHANNELS];
  long framesToCalc;
  long i;
  int finished;
  unsigned long framesLeft = data->maxFrameIndex - data->frameIndex;

  (void) outputBuffer; /* Prevent unused variable warnings. */
  (void) timeInfo;
  (void) statusFlags;
  (void) userData;

  if (framesLeft < framesPerBuffer) {
    framesToCalc = framesLeft;
    finished = paComplete;
  } else {
    framesToCalc = framesPerBuffer;
    finished = paContinue;
  }

  if (inputBuffer == NULL) {
    for (i = 0; i < framesToCalc; i++) {
      *wptr++ = SAMPLE_SILENCE;  /* left */
      if (NUM_CHANNELS == 2) *wptr++ = SAMPLE_SILENCE;  /* right */
    }
  } else {
    for (i = 0; i < framesToCalc; i++) {
      *wptr++ = *rptr++;  /* left */
      if (NUM_CHANNELS == 2) *wptr++ = *rptr++;  /* right */
    }
  }

  data->frameIndex += framesToCalc;
  //printf("%f\n",data->recordedSamples[0]);
  return finished;
}


/* This routine will be called by the PortAudio engine when audio is needed.
** It may be called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
[[noreturn]] static int playCallback(const void *inputBuffer, void *outputBuffer,
                        unsigned long framesPerBuffer,
                        const PaStreamCallbackTimeInfo *timeInfo,
                        PaStreamCallbackFlags statusFlags,
                        void *userData) {
  paTestData *data = (paTestData *) userData;
  SAMPLE *rptr = &data->recordedSamples[data->frameIndex * NUM_CHANNELS];
  SAMPLE *wptr = (SAMPLE *) outputBuffer;
  unsigned int i;
  int finished;
  unsigned int framesLeft = data->maxFrameIndex - data->frameIndex;
  
  (void) inputBuffer; /* Prevent unused variable warnings. */
  (void) timeInfo;
  (void) statusFlags;
  (void) userData;

  if (framesLeft < framesPerBuffer) {
    /* final buffer... */
    for (i = 0; i < framesLeft; i++) {
      *wptr++ = *rptr++;  /* left */
      if (NUM_CHANNELS == 2) *wptr++ = *rptr++;  /* right */
    }
    for (; i < framesPerBuffer; i++) {
      *wptr++ = CONVERT_IN_TO_OUT( i );
      //*wptr++ = 0;  /* left */
      if (NUM_CHANNELS == 2) *wptr++ = 0;  /* right */
    }
    data->frameIndex += framesLeft;
    finished = paComplete;
  } else {
    for (i = 0; i < framesPerBuffer; i++) {
      *wptr++ = CONVERT_IN_TO_OUT( *rptr++ );
      //*wptr++ = *rptr++;  /* left */
      if (NUM_CHANNELS == 2) *wptr++ = *rptr++;  /* right */
    }
    data->frameIndex += framesPerBuffer;
    //std::cout<< (abs(data->recordedSamples[0]))<< std::endl;
    //printf("%f\n",data->recordedSamples[0]);
    finished = paContinue;
  }
  return finished;
}


int main() {

  PaStreamParameters  inputParameters,
          outputParameters;
  PaStream*           stream;
  const PaDeviceInfo        *deviceInfo;
  PaError             err = paNoError;
  paTestData          data;
  int                 i;
  int                 totalFrames;
  int                 numSamples;
  int                 numBytes;
  SAMPLE              max, val;
  double              average;

  err = Pa_Initialize();

  if (err != paNoError) {
    printf("PortAudio error: %s\n", Pa_GetErrorText(err));
  } else {
    printf("No error");
  }

  data.maxFrameIndex = totalFrames = NUM_SECONDS * SAMPLE_RATE; /* Record for a few seconds. */
  data.frameIndex = 0;
  numSamples = totalFrames * NUM_CHANNELS;
  numBytes = numSamples * sizeof(SAMPLE);
  data.recordedSamples = (SAMPLE *) malloc( numBytes ); /* From now on, recordedSamples is initialised. */
  if( data.recordedSamples == NULL )
  {
    printf("Could not allocate record array.\n");
    goto done;
  }
  for( i=0; i<numSamples; i++ ) data.recordedSamples[i] = 0;

  err = Pa_Initialize();
  if( err != paNoError ) goto done;

  inputParameters.device = Pa_GetDefaultInputDevice(); /* default input device */
  if (inputParameters.device == paNoDevice) {
  fprintf(stderr,"Error: No default input device.\n");
  goto done;
  }
  deviceInfo = Pa_GetDeviceInfo(inputParameters.device);
  std::cout << deviceInfo->name  << std::endl;
  inputParameters.channelCount = 1;                    /* stereo input */
  inputParameters.sampleFormat = PA_SAMPLE_TYPE;
  inputParameters.suggestedLatency = Pa_GetDeviceInfo( inputParameters.device )->defaultLowInputLatency;
  inputParameters.hostApiSpecificStreamInfo = NULL;

  /* Record some audio. -------------------------------------------- */
  err = Pa_OpenStream(
          &stream,
          &inputParameters,
          NULL,                  /* &outputParameters, */
          SAMPLE_RATE,
          FRAMES_PER_BUFFER,
          paClipOff,      /* we won't output out of range samples so don't bother clipping them */
          recordCallback,
          &data );
  if( err != paNoError ) goto done;

  err = Pa_StartStream( stream );
  if( err != paNoError ) goto done;
  printf("\n=== Now recording!! Please speak into the microphone. ===\n"); fflush(stdout);

  while( ( err = Pa_IsStreamActive( stream ) ) == 1 )
  {
  Pa_Sleep(1000);
  printf("index = %d\n", data.frameIndex ); fflush(stdout);
  }
  if( err < 0 ) goto done;

  err = Pa_CloseStream( stream );
  if( err != paNoError ) goto done;

  /* Measure maximum peak amplitude. */
  max = 0;
  average = 0.0;
  for( i=0; i<numSamples; i++ )
  {
  val = data.recordedSamples[i];
  if( val < 0 ) val = -val; /* ABS */
  if( val > max )
  {
  max = val;
  }
  average += val;
  }

  average = average / (double)numSamples;

  printf("sample max amplitude = %.8f\n", max );
  printf("sample average = %lf\n", average );

/* Write recorded data to a file. */
#if WRITE_TO_FILE
{
        FILE  *fid;
        fid = fopen("recorded.raw", "wb");
        if( fid == NULL )
        {
            printf("Could not open file.");
        }
        else
        {
            fwrite( data.recordedSamples, NUM_CHANNELS * sizeof(SAMPLE), totalFrames, fid );
            fclose( fid );
            printf("Wrote data to 'recorded.raw'\n");
        }
    }
#endif

/* Playback recorded data.  -------------------------------------------- */
  data.frameIndex = 0;

  outputParameters.device = Pa_GetDefaultOutputDevice(); /* default output device */
  if (outputParameters.device == paNoDevice) {
  fprintf(stderr,"Error: No default output device.\n");
  goto done;
  }
  outputParameters.channelCount = 2;                     /* stereo output */
  outputParameters.sampleFormat =  PA_SAMPLE_TYPE;
  outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency;
  outputParameters.hostApiSpecificStreamInfo = NULL;

  printf("\n=== Now playing back. ===\n"); fflush(stdout);
  err = Pa_OpenStream(
          &stream,
          NULL, /* no input */
          &outputParameters,
          SAMPLE_RATE,
          FRAMES_PER_BUFFER,
          paClipOff,      /* we won't output out of range samples so don't bother clipping them */
          playCallback,
          &data );
  if( err != paNoError ) goto done;

  if( stream )
  {
  err = Pa_StartStream( stream );
  if( err != paNoError ) goto done;

  printf("Waiting for playback to finish.\n"); fflush(stdout);

  while( ( err = Pa_IsStreamActive( stream ) ) == 1 ) Pa_Sleep(100);
  if( err < 0 ) goto done;

  err = Pa_CloseStream( stream );
  if( err != paNoError ) goto done;

  printf("Done.\n"); fflush(stdout);
  }

  done:
  Pa_Terminate();
  if( data.recordedSamples )       /* Sure it is NULL or valid. */
  free( data.recordedSamples );
  if( err != paNoError )
  {
  fprintf( stderr, "An error occurred while using the portaudio stream\n" );
  fprintf( stderr, "Error number: %d\n", err );
  fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
  err = 1;          /* Always return 0 or 1, but no other return codes. */
  }
  return err;
}