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
    <aside id="errorPanel" role="alert" hidden style="position:relative;z-index:20;background:#4b1515;color:#fff;border:2px solid #ff7777;padding:12px;margin:12px auto;max-width:650px;text-align:left">
        <strong>Errors</strong>
        <button id="clearErrors" type="button">Clear errors</button>
        <ul id="errorList" style="max-height:180px;overflow:auto;overflow-wrap:anywhere"></ul>
    </aside>

    <div id="overlay">
        <h1>Click anywhere to start stream</h1>
        <p>Required by browser audio security policies</p>
    </div>

    <h2>ESP32 Live Audio Waveform</h2>
    <canvas id="waveform"></canvas>
    <div style="max-width:600px;margin:16px auto">
        <label for="boost">Listening volume / boost: <output id="boostValue">1.0x</output></label>
        <input id="boost" type="range" min="0" max="20" step="0.1" value="1" style="width:100%">
        <p>0x mutes; up to 20x boosts quiet audio. High boost can distort playback.
        Recordings and clipping diagnostics use the original microphone signal.</p>
        <p>Notebook recording: /record.wav?seconds=10 (1-60 seconds).
        Stop browser streaming before recording; this receiver serves one audio connection at a time.</p>
    </div>
    <p id="diagnostics">16 kHz mono / 16-bit PCM / 32 KB per second. Playback buffer: 250 ms.</p>
    <p>Buffer gaps suggest delivery or browser scheduling delays. Near-clipping samples suggest an overloaded signal.</p>
    <button onclick="location.reload()">Stop / reconnect</button>

    <script>
        function showError(message) {
            const panel = document.getElementById('errorPanel');
            const list = document.getElementById('errorList');
            const item = document.createElement('li');
            item.textContent = new Date().toLocaleTimeString() + ': ' + String(message);
            list.appendChild(item);
            while (list.children.length > 20) list.removeChild(list.firstElementChild);
            panel.hidden = false;
        }
        window.addEventListener('error', event => showError(event.message || 'Unexpected browser error'));
        window.addEventListener('unhandledrejection', event =>
            showError(event.reason?.message || event.reason || 'Unexpected asynchronous error'));
        document.getElementById('clearErrors').onclick = () => {
            document.getElementById('errorList').textContent = '';
            document.getElementById('errorPanel').hidden = true;
        };

        let audioCtx, analyser, listeningGain, isRunning = false;
        const boost = document.getElementById('boost');
        boost.addEventListener('input', () => {
            document.getElementById('boostValue').value = Number(boost.value).toFixed(1) + 'x';
            if (listeningGain && audioCtx.state !== 'closed') {
                listeningGain.gain.setTargetAtTime(Number(boost.value), audioCtx.currentTime, 0.02);
            }
        });
        const canvas = document.getElementById('waveform');
        const canvasCtx = canvas.getContext('2d');
        const overlay = document.getElementById('overlay');

        overlay.addEventListener('click', async () => {
            if (isRunning) return;
            isRunning = true;
            overlay.style.display = 'none';
            const diagnostics = document.getElementById('diagnostics');
            let reader;
            const sources = new Set();
            try {
                audioCtx = new (window.AudioContext || window.webkitAudioContext)();
                await audioCtx.resume();
                analyser = audioCtx.createAnalyser();
                analyser.fftSize = 2048;
                listeningGain = audioCtx.createGain();
                listeningGain.gain.value = Number(boost.value);
                analyser.connect(listeningGain);
                listeningGain.connect(audioCtx.destination);
                const response = await fetch('/audio', {cache: 'no-store'});
                if (!response.ok || !response.body) throw new Error('Audio request failed: ' + response.status);
                reader = response.body.getReader();
                let pending = new Uint8Array(0);
                let nextTime = 0, gaps = 0, clipped = 0, total = 0, peak = 0;
                drawWaveform();
                while (isRunning) {
                    const {value, done} = await reader.read();
                    if (done) throw new Error('Stream disconnected; click to reconnect');
                    const bytes = new Uint8Array(pending.length + value.length);
                    bytes.set(pending);
                    bytes.set(value, pending.length);
                    let offset = 0;
                    while (offset + 2048 <= bytes.length) {
                        const buffer = audioCtx.createBuffer(1, 1024, 16000);
                        const out = buffer.getChannelData(0);
                        const data = new DataView(bytes.buffer, offset, 2048);
                        for (let i = 0; i < 1024; i++) {
                            out[i] = data.getInt16(i * 2, true) / 32768;
                            const amplitude = Math.abs(out[i]);
                            peak = Math.max(peak, amplitude);
                            if (amplitude >= 0.98) clipped++;
                            total++;
                        }
                        if (!nextTime || nextTime < audioCtx.currentTime + 0.01) {
                            if (nextTime) gaps++;
                            nextTime = audioCtx.currentTime + 0.25;
                        }
                        if (nextTime - audioCtx.currentTime > 2) {
                            throw new Error('Playback backlog exceeds 2 seconds; reconnect');
                        }
                        const source = audioCtx.createBufferSource();
                        source.buffer = buffer;
                        source.connect(analyser);
                        sources.add(source);
                        source.onended = () => { source.disconnect(); sources.delete(source); };
                        source.start(nextTime);
                        nextTime += buffer.duration;
                        offset += 2048;
                    }
                    pending = bytes.slice(offset);
                    diagnostics.textContent = 'Buffered: ' + Math.max(0, (nextTime - audioCtx.currentTime) * 1000).toFixed(0) +
                        ' ms | Buffer gaps: ' + gaps + ' | Peak: ' + (peak * 100).toFixed(1) +
                        '% | Near clipping: ' + clipped + ' / ' + total + ' samples';
                }
            } catch (err) {
                diagnostics.textContent = err.message;
                showError(err.message);
            } finally {
                isRunning = false;
                if (reader) await reader.cancel().catch(() => {});
                for (const source of sources) { source.stop(); source.disconnect(); }
                if (audioCtx) await audioCtx.close();
                overlay.style.display = 'flex';
            }
        });

        function drawWaveform() {
            if (!isRunning || !analyser) return;
            requestAnimationFrame(drawWaveform);

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
        .dma_buf_count = 8,
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
    const bool recording = server.uri() == "/record.wav";
    uint32_t remainingSamples = 0;
    if (recording) {
        const String secondsText = server.hasArg("seconds") ? server.arg("seconds") : "10";
        char* end = nullptr;
        const long seconds = strtol(secondsText.c_str(), &end, 10);
        if (end == secondsText.c_str() || *end != '\0' || seconds < 1 || seconds > 60) {
            server.send(400, "text/plain", "seconds must be an integer from 1 to 60");
            return;
        }
        remainingSamples = seconds * 16000;
    }
    WiFiClient client = server.client();
    client.println("HTTP/1.1 200 OK");
    client.println(recording ? "Content-Type: audio/wav" : "Content-Type: application/octet-stream");
    if (recording) {
        client.println("Content-Length: " + String(44 + remainingSamples * 2));
        client.println("Content-Disposition: attachment; filename=recording.wav");
    }
    client.println("Cache-Control: no-cache");
    client.println("Connection: close");
    client.println();

    client.setNoDelay(true);
    if (recording) {
        uint8_t header[44] = {};
        memcpy(header, "RIFF", 4);
        memcpy(header + 8, "WAVEfmt ", 8);
        memcpy(header + 36, "data", 4);
        auto put16 = [&](int offset, uint16_t value) {
            header[offset] = value & 255; header[offset + 1] = value >> 8;
        };
        auto put32 = [&](int offset, uint32_t value) {
            for (int i = 0; i < 4; ++i) header[offset + i] = (value >> (8 * i)) & 255;
        };
        put32(4, 36 + remainingSamples * 2);
        put32(16, 16); put16(20, 1); put16(22, 1);
        put32(24, 16000); put32(28, 32000);
        put16(32, 2); put16(34, 16); put32(40, remainingSamples * 2);
        size_t sentHeader = 0;
        while (sentHeader < sizeof(header)) {
            const size_t n = client.write(header + sentHeader, sizeof(header) - sentHeader);
            if (!n) { client.stop(); return; }
            sentHeader += n;
        }
    }
    int32_t input[256];
    int16_t pcm[256];
    uint32_t lastLog = millis();
    int32_t peak = 0;
    uint32_t clipped = 0;

    while (client.connected() && (!recording || remainingSamples > 0)) {
        size_t bytesRead = 0;
        if (i2s_read(I2S_NUM_0, input, sizeof(input), &bytesRead, pdMS_TO_TICKS(250)) != ESP_OK) break;
        size_t count = bytesRead / sizeof(int32_t);
        if (recording && count > remainingSamples) count = remainingSamples;
        if (!count) break;
        for (size_t i = 0; i < count; ++i) {
            // Preserve the existing assumption: signed 24-bit microphone data,
            // left-aligned in a 32-bit I2S word. Send the upper 16 bits.
            const int32_t sample = input[i] >> 8;
            peak = max(peak, abs(sample));
            if (abs(sample) >= 8220835) ++clipped;
            pcm[i] = static_cast<int16_t>(input[i] >> 16);
        }
        size_t sent = 0;
        const size_t length = count * sizeof(int16_t);
        while (sent < length && client.connected()) {
            const size_t written = client.write(reinterpret_cast<const uint8_t*>(pcm) + sent, length - sent);
            if (!written) { client.stop(); break; }
            sent += written;
        }
        if (recording) remainingSamples -= count;
        if (millis() - lastLog >= 1000) {
            Serial.printf("I2S peak: %ld / 8388608, near-clipping samples: %lu, RSSI: %d dBm\n",
                (long)peak, (unsigned long)clipped, WiFi.RSSI());
            peak = 0;
            clipped = 0;
            lastLog = millis();
        }
        yield();
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

    WiFi.setSleep(false);
    setupI2S();

    server.on("/", HTTP_GET, handleRoot);
    server.on("/audio", HTTP_GET, handleAudio);
    server.on("/record.wav", HTTP_GET, handleAudio);
    server.begin();
}

void loop() {
    server.handleClient();
}
