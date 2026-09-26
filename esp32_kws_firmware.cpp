#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

// FreeRTOS OS dependencies
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// TensorFlow Lite Micro dependencies
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"

#define SAMPLE_RATE 16000
#define CHUNK_SAMPLES 800      // 50ms of audio exactly (5 frames)
#define N_MELS 40
#define NUM_FRAMES 100         // 1.0 second context window
#define MATRIX_SIZE (NUM_FRAMES * N_MELS) // 4000 bytes

// Thresholds (To be tuned in the actual room environment)
#define TIER0_ENERGY_THRESHOLD 150000 
#define TIER1_VAD_THRESHOLD 50        

// Force weights into External PSRAM to save internal RAM
const unsigned char g_model[] __attribute__((section(".ext_ram.bss"))) = { 0x00 }; 

// Tensor Arena locked to fast internal SRAM, aligned for SIMD vector math
uint8_t tensor_arena[65 * 1024] __attribute__((aligned(16)));

tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;

// OS Primitives
SemaphoreHandle_t dsp_semaphore;
QueueHandle_t network_event_queue;

// Data Buffers
int16_t current_50ms_audio[CHUNK_SAMPLES];
int8_t sliding_pcen_matrix[MATRIX_SIZE]; // The 100x40 2D Grid

typedef struct {
    bool trigger;
    float confidence;
} KWS_Event;

// Hardware ISR triggered by I2S DMA every 50ms (800 samples)
void dma_audio_callback(int16_t* dma_buffer) {
    // Copy fresh 50ms chunk from DMA peripheral
    memcpy(current_50ms_audio, dma_buffer, CHUNK_SAMPLES * sizeof(int16_t));
    
    // Wake up the main processing task
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(dsp_semaphore, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

// TIER-0: Checks raw audio amplitude to completely bypass DSP if room is silent
bool tier0_energy_check(int16_t* audio_chunk, int num_samples) {
    long amplitude_sum = 0;
    for(int i = 0; i < num_samples; i++) {
        amplitude_sum += abs(audio_chunk[i]);
    }
    return (amplitude_sum > TIER0_ENERGY_THRESHOLD);
}

// MOCK: DSP teammate's function. Converts 50ms audio into exactly 5 PCEN vectors.
extern void extract_5_pcen_frames(int16_t* audio_in, int8_t* pcen_out);

// TIER-1: Checks a single 10ms PCEN frame (40 mels) for vocal frequencies
bool tier1_vad_check(int8_t* pcen_frame) {
    long energy = 0;
    // Bins 5 to 35 represent human speech formants (skips extreme rumble & hiss)
    for(int i = 5; i < 35; i++) { 
        energy += abs(pcen_frame[i]); 
    }
    return (energy > TIER1_VAD_THRESHOLD);
}

int consecutive_hits = 0;
bool is_cooldown = false;
TickType_t cooldown_start_time = 0;

void process_bouncer_logic() {
    // 1. Enforce Cooldown Period (1.5 seconds)
    if (is_cooldown) {
        if ((xTaskGetTickCount() - cooldown_start_time) < pdMS_TO_TICKS(1500)) return;
        is_cooldown = false; 
    }

    // 2. Dequantize INT8 output to Float32 Confidence percentage
    TfLiteTensor* output = interpreter->output(0);
    float confidence = output->params.scale * (output->data.int8[0] - output->params.zero_point);

    // 3. Multi-frame sliding window bouncer
    if (confidence > 0.85f) {
        consecutive_hits++;
    } else {
        if (consecutive_hits > 0) consecutive_hits--;
    }

    // 4. Trigger Network Handoff
    if (consecutive_hits >= 3) {
        consecutive_hits = 0;
        is_cooldown = true;
        cooldown_start_time = xTaskGetTickCount();
        
        KWS_Event event = { .trigger = true, .confidence = confidence };
        xQueueSend(network_event_queue, &event, 0);
    }
}

void kws_main_task(void *pvParameters) {
    // Initialize empty matrix
    memset(sliding_pcen_matrix, 0, MATRIX_SIZE);

    while (1) {
        // Sleep at 0% CPU until 50ms of audio is ready
        if (xSemaphoreTake(dsp_semaphore, portMAX_DELAY)) {
            
            // 1. Shift the matrix left by 5 frames (Make room for new data)
            // Move elements from index 200 (frame 5) to index 0 (frame 0)
            memmove(sliding_pcen_matrix, 
                    sliding_pcen_matrix + (5 * N_MELS), 
                    (NUM_FRAMES - 5) * N_MELS * sizeof(int8_t));

            // Pointer to the start of the 5 empty frames at the end (indices 95 to 99)
            int8_t* newest_5_frames = &sliding_pcen_matrix[95 * N_MELS];

            // 2. TIER-0 GATE (Raw Audio)
            if (!tier0_energy_check(current_50ms_audio, CHUNK_SAMPLES)) {
                // SILENCE DETECTED: Bypass DSP math!
                // Fix: Pad by duplicating the last known valid frame (Frame 94) 5 times
                int8_t* last_valid_frame = &sliding_pcen_matrix[94 * N_MELS];
                for(int i = 0; i < 5; i++) {
                    memcpy(newest_5_frames + (i * N_MELS), last_valid_frame, N_MELS);
                }
            } else {
                // SOUND DETECTED: Run heavy DSP math and generate 5 INT8 vectors
                extract_5_pcen_frames(current_50ms_audio, newest_5_frames);
            }

            // 3. TIER-1 GATE (Vocal Frequencies) & The "1 out of 5" Rule
            bool speech_present = false;
            for (int f = 0; f < 5; f++) {
                // Check each of the 5 new frames
                if (tier1_vad_check(newest_5_frames + (f * N_MELS))) {
                    speech_present = true;
                    break; // AT LEAST ONE PASSED! Stop checking.
                }
            }

            // 4. NEURAL NETWORK INVOCATION
            if (speech_present) { 
                // Copy the ENTIRE 1.0-second context window into the Tensor Arena
                memcpy(input->data.int8, sliding_pcen_matrix, MATRIX_SIZE);
                
                // Hardware Accelerated SIMD Math
                interpreter->Invoke();
                
                // Route output to Leaky Bouncer
                process_bouncer_logic();
            }
        }
    }
}