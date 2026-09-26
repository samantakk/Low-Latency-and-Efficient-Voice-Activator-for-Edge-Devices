import os
import numpy as np
import librosa
import tensorflow as tf
from sklearn.model_selection import train_test_split

# ============================================================
# 1. GLOBAL CONFIGURATION (Must match C++ DSP exactly)
# ============================================================
SEED = 42
np.random.seed(SEED)
tf.random.set_seed(SEED)

SAMPLE_RATE = 16000
CLIP_SAMPLES = SAMPLE_RATE            # Exactly 1.0 second
WINDOW_SAMPLES = 400                  # 25 ms window
HOP_SAMPLES = 160                     # 10 ms hop -> 100 frames per sec
FFT_SIZE = 512
N_MELS = 40

# PCEN Constants (Matches C++ esp-dsp variables)
PCEN_GAIN = 0.98
PCEN_BIAS = 2.0
PCEN_POWER = 0.5
PCEN_TIME_CONSTANT = 0.4
PCEN_EPS = 1e-6

INPUT_SHAPE = (100, N_MELS, 1)

# ============================================================
# 2. DIGITAL TWIN: C++ EXACT REPLICA DSP PIPELINE
# ============================================================
def pcen_recursive(mel_power):
    """
    Simulates the exact step-by-step math of the ESP-DSP C++ library.
    We do NOT use librosa.pcen() because it looks at the whole file at once.
    This recursive filter guarantees Train-to-Firmware Feature Parity.
    """
    n_mels, n_frames = mel_power.shape
    output = np.zeros_like(mel_power, dtype=np.float32)
    
    # Calculate smoothing factor 's'
    smoothing = 1.0 - np.exp(-float(HOP_SAMPLES) / (float(SAMPLE_RATE) * PCEN_TIME_CONSTANT))
    smoother = mel_power[:, 0].copy()

    for t in range(n_frames):
        if t > 0:
            smoother = ((1.0 - smoothing) * smoother) + (smoothing * mel_power[:, t])
        
        normalized = mel_power[:, t] / np.power(PCEN_EPS + smoother, PCEN_GAIN)
        output[:, t] = np.power(normalized + PCEN_BIAS, PCEN_POWER) - np.power(PCEN_BIAS, PCEN_POWER)
        
    return output

def extract_features(audio_path):
    """ Converts raw audio to 100x40 PCEN Matrix """
    y, _ = librosa.load(audio_path, sr=SAMPLE_RATE, mono=True)
    y = np.nan_to_num(y.astype(np.float32))

    # Pad or trim to exactly 1.0 second
    if len(y) < CLIP_SAMPLES:
        y = np.pad(y, (0, CLIP_SAMPLES - len(y)), mode="constant")
    else:
        y = y[:CLIP_SAMPLES]

    # STFT -> Mel Filterbank -> Custom PCEN
    stft = librosa.stft(y, n_fft=FFT_SIZE, hop_length=HOP_SAMPLES, win_length=WINDOW_SAMPLES, window="hann", center=False)
    power_spectrum = np.abs(stft) ** 2
    
    mel_filterbank = librosa.filters.mel(sr=SAMPLE_RATE, n_fft=FFT_SIZE, n_mels=N_MELS, fmin=20.0, fmax=4000.0, norm="slaney")
    mel_power = np.matmul(mel_filterbank, power_spectrum).astype(np.float32)
    
    pcen_frames = pcen_recursive(mel_power).T  # Shape: (frames, 40)

    # Force exactly 100 frames (Duplicate first frame for warmup padding if needed)
    if pcen_frames.shape[0] < 100:
        missing = 100 - pcen_frames.shape[0]
        warmup = np.repeat(pcen_frames[0:1], missing, axis=0)
        pcen_frames = np.concatenate([warmup, pcen_frames], axis=0)
    elif pcen_frames.shape[0] > 100:
        pcen_frames = pcen_frames[-100:]

    return np.expand_dims(pcen_frames, axis=-1)

# ============================================================
# 3. DATASET LOADING
# ============================================================
def load_dataset():
    """
    Assumes directory structure:
    /data/positives/ (Your team's custom wake word)
    /data/negatives/ (Google Speech Commands unknowns + background noise)
    """
    X_data, y_labels = [], []
    print("Extracting features... This takes time!")
    
    # Dummy data generator for structural testing (Replace with actual file loops)
    # FOR SIH: Uncomment the file parsing logic in production
    '''
    for file in os.listdir("data/positives/"):
        if file.endswith('.wav'):
            X_data.append(extract_features(os.path.join("data/positives", file)))
            y_labels.append(1)
            
    for file in os.listdir("data/negatives/"):
        if file.endswith('.wav'):
            X_data.append(extract_features(os.path.join("data/negatives", file)))
            y_labels.append(0)
    '''
    
    # Generating mock matrices so this script runs out-of-the-box for verification
    X_data = np.random.rand(200, 100, 40, 1).astype(np.float32) 
    y_labels = np.random.randint(0, 2, 200)
    
    return np.array(X_data), np.array(y_labels)

# ============================================================
# 4. BC-RESNET ARCHITECTURE
# ============================================================
def bc_res_block(x, filters):
    """
    The Decoupled Block: Splits 2D math into 1D Space + 1D Time
    Saves ~40x the CPU Operations of standard CNNs.
    """
    shortcut = x
    if x.shape[-1] != filters:
        shortcut = tf.keras.layers.Conv2D(filters, (1, 1), padding="same", use_bias=False)(shortcut)
        shortcut = tf.keras.layers.BatchNormalization()(shortcut)

    # 1. Frequency Analysis (f2) - 3x1 kernel sliding up/down pitch axis
    y = tf.keras.layers.DepthwiseConv2D((3, 1), padding="same", use_bias=False)(x)
    y = tf.keras.layers.BatchNormalization()(y)
    y = tf.keras.layers.ReLU()(y)
    y = tf.keras.layers.Conv2D(filters, (1, 1), padding="same", use_bias=False)(y)
    y = tf.keras.layers.BatchNormalization()(y)
    y = tf.keras.layers.ReLU()(y)

    # 2. Time Analysis (f1) - 1x3 kernel sliding across time
    y = tf.keras.layers.DepthwiseConv2D((1, 3), padding="same", use_bias=False)(y)
    y = tf.keras.layers.BatchNormalization()(y)
    y = tf.keras.layers.ReLU()(y)
    y = tf.keras.layers.Conv2D(filters, (1, 1), padding="same", use_bias=False)(y)
    y = tf.keras.layers.BatchNormalization()(y)

    # 3. Residual Broadcast
    y = tf.keras.layers.Add()([shortcut, y])
    return tf.keras.layers.ReLU()(y)

def build_bc_resnet(input_shape=INPUT_SHAPE):
    inp = tf.keras.layers.Input(shape=input_shape)
    
    # Stem
    x = tf.keras.layers.Conv2D(8, (5, 5), strides=(2, 1), padding="same", use_bias=False)(inp)
    x = tf.keras.layers.BatchNormalization()(x)
    x = tf.keras.layers.ReLU()(x)

    # BC-ResNet Blocks (Shrinking spatial dims, growing channels)
    x = bc_res_block(x, 12)
    x = bc_res_block(x, 16)
    x = bc_res_block(x, 24)

    # Bottleneck before classification
    x = tf.keras.layers.GlobalAveragePooling2D()(x)
    x = tf.keras.layers.Dropout(0.2)(x)
    out = tf.keras.layers.Dense(1, activation="sigmoid")(x)
    
    return tf.keras.models.Model(inp, out)

# ============================================================
# 5. TRAINING PHASE
# ============================================================
print("Loading data...")
X_data, y_labels = load_dataset()
X_train, X_test, y_train, y_test = train_test_split(X_data, y_labels, test_size=0.2, random_state=SEED)

print("Building Model...")
model = build_bc_resnet()
model.compile(optimizer='adam', loss='binary_crossentropy', metrics=['accuracy'])
model.summary()

print("Training Model (FP32)...")
# Train the float32 model normally
model.fit(X_train, y_train, validation_data=(X_test, y_test), epochs=5, batch_size=32)

# ============================================================
# 6. POST-TRAINING INT8 QUANTIZATION (The SIH Magic)
# ============================================================
print("\nQuantizing to INT8 for ESP32-S3...")

def representative_data_gen():
    """ Feeds samples to TFLite so it learns the real-world scale and zero-point. """
    for input_value in tf.data.Dataset.from_tensor_slices(X_train).batch(1).take(150):
        yield [tf.cast(input_value, tf.float32)]

converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = representative_data_gen
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]

# FORCE fully quantized I/O so the C++ arrays can just use int8_t directly
converter.inference_input_type = tf.int8
converter.inference_output_type = tf.int8

tflite_model = converter.convert()

with open("kws_model_int8.tflite", "wb") as f:
    f.write(tflite_model)
print(f"Quantized Model Saved! Size: {len(tflite_model)/1024:.2f} KB (Target < 50KB)")

# ============================================================
# 7. EXPORT TO ESP32 C++ HEADER
# ============================================================
def write_c_header(tflite_binary, output_file="kws_model_data.h"):
    """ Converts the .tflite binary into a C-array that goes straight into PSRAM """
    with open(output_file, "w") as f:
        f.write("/* Auto-generated for SIH ESP32-S3 Firmware */\n")
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        
        # alignas(16) is REQUIRED for ESP32-S3 SIMD instructions!
        f.write("alignas(16) const unsigned char g_model[] __attribute__((section(\".ext_ram.bss\"))) = {\n")
        
        for i in range(0, len(tflite_binary), 12):
            chunk = tflite_binary[i:i+12]
            values = ", ".join(f"0x{b:02x}" for b in chunk)
            f.write(f"    {values},\n")
            
        f.write("};\n\n")
        f.write(f"const unsigned int g_model_len = {len(tflite_binary)};\n")
        
    print(f"C++ Header generated successfully: {output_file}")

write_c_header(tflite_model)
print("Pipeline Complete! Hand the .h file to the DSP team.")