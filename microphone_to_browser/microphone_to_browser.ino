#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "driver/i2s.h"

const char* ssid = "OpenWrt2.4";
const char* password = "narrowboat564";

WebServer server(80);

#define I2S_WS      25
#define I2S_SD      33
#define I2S_SCK     32

const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>ESP32 Audio Stream</title>
    <style>
        body { font-family: sans-serif; text-align: center; background: #111; color: #fff; margin-top: 50px; }
        canvas { background: #222; border: 1px solid #444; width: 80%; max-width: 600px; height: 200px; }
        #overlay { position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.85); display: flex; flex-direction: column; justify-content: center; align-items: center; z-index: 10; cursor: pointer; }
        h1 { font-size: 24px; color: #00ffcc; }
        p { color: #aaa; }
    </style>
</head>
<body>
    <div id="overlay">
        <h1>Click anywhere to start stream</h1>
        <p>Required by browser audio security policies</p>
    </div>

    <h2>ESP32 Live Audio Waveform</h2>
    <canvas id="waveform"></canvas>

    <script>
        let audioCtx, analyser, isRunning = false;
        const canvas = document.getElementById('waveform');
        const canvasCtx = canvas.getContext('2d');
        const overlay = document.getElementById('overlay');

        overlay.addEventListener('click', async () => {
            overlay.style.display = 'none';
            if (isRunning) return;
            isRunning = true;

            audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 16000 });
            analyser = audioCtx.createAnalyser();
            analyser.fftSize = 2048;

            const response = await fetch('/audio');
            if (!response.ok || !response.body) {
                throw new Error(`Audio request failed: ${response.status}`);
            }
            const reader = response.body.getReader();

            const bufferSize = 2048;
            const scriptNode = audioCtx.createScriptProcessor(bufferSize, 0, 1);
            let pcmQueue = new Float32Array(0);
            let pendingBytes = new Uint8Array(0);

            scriptNode.onaudioprocess = function(e) {
                const outputData = e.outputBuffer.getChannelData(0);
                if (pcmQueue.length >= outputData.length) {
                    outputData.set(pcmQueue.subarray(0, outputData.length));
                    pcmQueue = pcmQueue.subarray(outputData.length);
                } else {
                    outputData.set(pcmQueue);
                    outputData.fill(0, pcmQueue.length);
                    pcmQueue = new Float32Array(0);
                }
            };

            scriptNode.connect(analyser);
            analyser.connect(audioCtx.destination);

            (async () => {
                while (true) {
                    const { value, done } = await reader.read();
                    if (done) break;

                    const bytes = new Uint8Array(pendingBytes.length + value.length);
                    bytes.set(pendingBytes);
                    bytes.set(value, pendingBytes.length);
                    const sampleBytes = bytes.length - (bytes.length % 4);
                    pendingBytes = bytes.slice(sampleBytes);

                    const int32View = new DataView(bytes.buffer, bytes.byteOffset, sampleBytes);
                    const float32Chunk = new Float32Array(sampleBytes / 4);
                    for (let i = 0; i < float32Chunk.length; i++) {
                        // Right shift by 8 for 24-bit data padded in 32-bit I2S frames
                        let sample = int32View.getInt32(i * 4, true) >> 8;
                        float32Chunk[i] = sample / 8388608.0; 
                    }

                    const newQueue = new Float32Array(pcmQueue.length + float32Chunk.length);
                    newQueue.set(pcmQueue);
                    newQueue.set(float32Chunk, pcmQueue.length);
                    pcmQueue = newQueue;
                }
            })();

            drawWaveform();
        });

        function drawWaveform() {
            requestAnimationFrame(drawWaveform);
            if (!analyser) return;

            const bufferLength = analyser.frequencyBinCount;
            const dataArray = new Uint8Array(bufferLength);
            analyser.getByteTimeDomainData(dataArray);

            canvas.width = canvas.offsetWidth;
            canvas.height = canvas.offsetHeight;

            canvasCtx.fillStyle = '#222';
            canvasCtx.fillRect(0, 0, canvas.width, canvas.height);

            canvasCtx.lineWidth = 2;
            canvasCtx.strokeStyle = '#00ffcc';
            canvasCtx.beginPath();

            const sliceWidth = canvas.width * 1.0 / bufferLength;
            let x = 0;

            for (let i = 0; i < bufferLength; i++) {
                const v = dataArray[i] / 128.0;
                const y = v * canvas.height / 2;

                if (i === 0) {
                    canvasCtx.moveTo(x, y);
                } else {
                    canvasCtx.lineTo(x, y);
                }
                x += sliceWidth;
            }

            canvasCtx.lineTo(canvas.width, canvas.height / 2);
            canvasCtx.stroke();
        }
    </script>
</body>
</html>
)rawliteral";

void setupI2S() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
}

void handleRoot() {
    server.send(200, "text/html", PAGE_HTML);
}

void handleAudio() {
    WiFiClient client = server.client();
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/octet-stream");
    client.println("Cache-Control: no-cache");
    client.println("Connection: close");
    client.println();

    uint8_t buffer[512];
    size_t bytes_read;
    int32_t peak = 0;

    while (client.connected()) {
        i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytes_read, portMAX_DELAY);
        for (size_t i = 0; i + 3 < bytes_read; i += 4) {
            int32_t sample;
            memcpy(&sample, buffer + i, sizeof(sample));
            sample >>= 8;
            if (abs(sample) > peak) peak = abs(sample);
        }
        if (peak > 0) {
            Serial.printf("I2S peak: %ld\n", (long)peak);
            peak = 0;
        }
        client.write(buffer, bytes_read);
    }
    client.stop();
}

void setup() {
    Serial.begin(115200);

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
    }
    Serial.println("\nConnected!");
    Serial.println(WiFi.localIP());

    setupI2S();

    server.on("/", HTTP_GET, handleRoot);
    server.on("/audio", HTTP_GET, handleAudio);
    server.begin();
}

void loop() {
    server.handleClient();
}
