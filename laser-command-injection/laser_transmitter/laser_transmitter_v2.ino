#include <Arduino.h>
#include <math.h>
#include <esp_timer.h>
#include <WiFi.h>
#include <WebServer.h>

const char* ssid = "OpenWrt2.4";
const char* password = "narrowboat564";

WebServer server(80);

// Laser modulation pin (PWM output to laser TTL input)
// XIAO ESP32-C3 board labels D0 through D10, not raw GPIO numbers.
const uint8_t DIGITAL_PINS[] = {2, 3, 4, 5, 6, 7, 21, 20, 8, 9, 10};
uint8_t selectedPin = 5;  // D5 = GPIO7 (also SCL when used for I2C)
#define LASER_PIN (DIGITAL_PINS[selectedPin])
enum OutputMode { OUTPUT_OFF, OUTPUT_TEST, OUTPUT_AUDIO };
OutputMode outputMode = OUTPUT_OFF;
bool pwmAttached = false;
uint32_t testStarted = 0;
uint32_t testPeriodUs = 1000000;
uint8_t testPwmPins = 0;
double testHz = 1.0;
#define PWM_FREQ    15000  // 15 kHz carrier frequency for laser TTL
#define PWM_CHANNEL 0
#define PWM_RES     8      // 8-bit resolution (0-255)

volatile uint8_t outputLevel = 100;
volatile uint8_t modulationMode = 0;

// Volatile clip storage: 8 seconds, 16 kHz unsigned 8-bit mono PCM.
constexpr size_t MAX_CLIP_BYTES = 128000;
uint8_t clipBuffer[MAX_CLIP_BYTES];
size_t clipLength = 0;
size_t uploadLength = 0;
size_t uploadExpected = 0;
bool clipPlaying = false;
int64_t clipStarted = 0;
bool clipRepeat = true;
size_t lastClipSample = MAX_CLIP_BYTES;
uint8_t audioVolume = 100;
esp_timer_handle_t audioTimer = nullptr;
volatile size_t audioSampleIndex = 0;
volatile bool audioFinished = false;

const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>ESP32-C3 Laser Audio Transmitter</title>
    <style>
        body { font-family: sans-serif; text-align: center; background: #111; color: #fff; margin-top: 50px; }
        canvas { background: #222; border: 1px solid #444; width: 80%; max-width: 600px; height: 160px; }
        h1 { font-size: 24px; color: #00ff00; }
        p { color: #aaa; }
        #status { margin-top: 20px; padding: 10px; background: #1a1a1a; border-radius: 5px; }
        .transmitting { color: #00ff00; }
        .error { color: #ff3333; }
        .control { margin: 12px auto; max-width: 600px; text-align: left; }
        .control label { display: block; margin: 8px 0 4px; }
        input[type="range"], select { width: 100%; }
    </style>
</head>
<body>
    <aside id="errorPanel" role="alert" hidden style="position:relative;z-index:20;background:#4b1515;color:#fff;border:2px solid #ff7777;padding:12px;margin:12px auto;max-width:650px;text-align:left">
        <strong>Errors</strong>
        <button id="clearErrors" type="button">Clear errors</button>
        <ul id="errorList" style="max-height:180px;overflow:auto;overflow-wrap:anywhere"></ul>
    </aside>

<h2>Laser Audio Transmitter</h2>
    <div class="control">
        <label for="outputPin">Output pin (XIAO ESP32-C3)</label>
        <select id="outputPin"></select>
        <p>D0, D8 and D9 affect boot. D6/D7 share UART pins. Disconnect other peripherals before testing all pins.
        D1 with an external pull-down is a better starting point for default-low control.</p>
        <label for="testHz">Test frequency (0.1 to 15000 Hz)</label>
        <input id="testHz" type="number" min="0.1" max="15000" step="any" value="1">
        <input id="testHzSlider" aria-label="Test frequency slider" type="range" min="0" max="1000" step="1">
        <button id="startTest">Start continuous test on ALL digital pins</button>
        <button id="stopOutput">Stop / output LOW</button>

        <p>Test drives D0-D10 together: 50% HIGH / 50% LOW, repeating until stopped. The pin selector applies to audio.
        Uses full 3.3 V logic; the audio level slider does not affect this test.</p>
    </div>
    <p>Transmitted audio envelope</p>
    <canvas id="waveform"></canvas>
    <p>Laser TTL pin preview, same time scale</p>
    <canvas id="pinWaveform"></canvas>
    <div id="status">Status: <span id="statusText">Ready</span></div>
    <p>
        <input id="audioFile" type="file" accept="audio/*">
        <button id="sendFile">Upload audio file</button>
        <button id="playClip">Play uploaded audio</button>
        <label><input id="repeatClip" type="checkbox" checked> Repeat continuously (applies on next Play)</label>
        <label><input id="monitorEnabled" type="checkbox" checked> Hear audio in this browser</label>
        <p>Browser monitor plays the stored audio with audio volume applied.
        It does not measure the laser or reproduce modulation distortion. Network/audio latency may offset playback slightly.</p>
        <p id="fileInfo">Maximum 8 seconds; choose a browser-supported WAV, MP3 or other audio file. Upload is held in RAM until restart.</p>
        <button id="send440">Upload 440 Hz tone</button>
        <button id="send1000">Upload 1 kHz tone</button>
    </p>
    <div class="control">
        <label for="level">Laser level: <output id="levelValue">100%</output></label>
        <input id="level" type="range" min="0" max="100" value="100">
        <label for="volume">Audio volume: <output id="volumeValue">100%</output></label>
        <input id="volume" type="range" min="0" max="100" value="100">
        <label for="modulation">Modulation mode</label>
        <select id="modulation">
            <option value="0">Direct PWM level</option>
            <option value="1">Centered PWM audio depth</option>
            <option value="2">1-bit on/off threshold</option>
            <option value="3">4-level quantized PWM</option>
        </select>
        <label for="timeScale">Rolling window: <output id="timeScaleValue">5 ms</output></label>
        <input id="timeScale" type="range" min="0.5" max="10000" step="0.5" value="5">
    </div>

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

        let audioCtx;
        let monitorSource = null, monitorGain = null, monitorBuffer = null;
        let preparingPlayback = false;
        let playbackRepeat = true;
        function stopMonitor() {
            if (monitorSource) {
                monitorSource.onended = null;
                monitorSource.stop();
                monitorSource.disconnect();
                monitorSource = null;
            }
            if (monitorGain) { monitorGain.disconnect(); monitorGain = null; }
        }
        function startMonitor() {
            stopMonitor();
            if (!monitorBuffer || !document.getElementById('monitorEnabled').checked) return;
            monitorGain = audioCtx.createGain();
            monitorGain.gain.value = Number(volume.value) / 100;
            monitorSource = audioCtx.createBufferSource();
            monitorSource.buffer = monitorBuffer;
            monitorSource.loop = playbackRepeat;
            monitorSource.connect(monitorGain);
            monitorGain.connect(audioCtx.destination);
            monitorSource.onended = () => {
                stopMonitor();
            };
            monitorSource.start();
        }
        let uploadBusy = false;
        let operation = 0;
        let previewSamples = null;
        let previewPosition = 0;
        const canvas = document.getElementById('waveform');
        const canvasCtx = canvas.getContext('2d');
        const pinCanvas = document.getElementById('pinWaveform');
        const pinCanvasCtx = pinCanvas.getContext('2d');
        const statusText = document.getElementById('statusText');
        const level = document.getElementById('level');
        const levelValue = document.getElementById('levelValue');
        const volume = document.getElementById('volume');
        const volumeValue = document.getElementById('volumeValue');
        const modulation = document.getElementById('modulation');
        const timeScale = document.getElementById('timeScale');
        const timeScaleValue = document.getElementById('timeScaleValue');

        const outputPin = document.getElementById('outputPin');
        [2, 3, 4, 5, 6, 7, 21, 20, 8, 9, 10].forEach((gpio, index) => {
            outputPin.add(new Option('D' + index + ' / GPIO' + gpio, index));
        });
        outputPin.value = '5';
        const testHzInput = document.getElementById('testHz');
        const testHzSlider = document.getElementById('testHzSlider');
        let testActive = false;
        let frequencyTimer;
        let outputQueue = Promise.resolve();
        function syncFrequencySlider() {
            testHzSlider.value = 1000 * Math.log(Number(testHzInput.value) / 0.1) / Math.log(150000);
        }
        syncFrequencySlider();
        function frequencyChanged() {
            clearTimeout(frequencyTimer);
            if (testActive) frequencyTimer = setTimeout(() => controlOutput('test'), 200);
        }
        testHzSlider.addEventListener('input', () => {
            testHzInput.value = Number((0.1 * Math.pow(150000, Number(testHzSlider.value) / 1000)).toPrecision(5));
            frequencyChanged();
        });
        testHzInput.addEventListener('input', () => {
            if (testHzInput.validity.valid && testHzInput.value) {
                syncFrequencySlider();
                frequencyChanged();
            }
        });
        function controlOutput(mode) {
            ++operation;
            stopMonitor();
            clearTimeout(frequencyTimer);
            const hz = Number(testHzInput.value);
            if (mode === 'test' && (!Number.isFinite(hz) || hz < 0.1 || hz > 15000)) {
                setStatus('Enter a frequency from 0.1 to 15000 Hz', 'error');
                return;
            }
            testActive = mode === 'test';
            const pin = outputPin.value;
            const token = operation;
            const repeat = document.getElementById('repeatClip').checked;
            outputQueue = outputQueue.then(() => sendOutputControl(mode, pin, hz, token, repeat));
            return outputQueue;
        }
        async function sendOutputControl(mode, pin, hz, token, repeat) {
            if (token !== operation) return false;
            try {
                const response = await fetch('/output?pin=' + pin + '&mode=' + mode + '&hz=' + hz + '&repeat=' + (repeat ? '1' : '0'),
                    { method: 'POST' });
                const message = await response.text();
                if (!response.ok) throw new Error(message);
                if (token !== operation) return false;
                if (mode === 'audio') { playbackRepeat = repeat; startMonitor(); }
                setStatus(message);
                return true;
            } catch (err) { setStatus(err.message, 'error'); }
        }
        outputPin.addEventListener('change', () => controlOutput('off'));
        document.getElementById('startTest').onclick = () => controlOutput('test');
        document.getElementById('stopOutput').onclick = () => controlOutput('off');

        function setStatus(message, className = '') {
            if (className === 'error') showError(message);
            statusText.textContent = message;
            statusText.className = className;
        }

        function modulationDuty(sample) {
            sample = 128 + Math.trunc((sample - 128) * Number(volume.value) / 100);
            const levelValue = Number(level.value);
            let duty;
            switch (Number(modulation.value)) {
                case 1:
                    duty = (128 + ((sample - 128) * levelValue) / 100)
                        * levelValue / 100;
                    break;
                case 2:
                    duty = sample > 128 ? (255 * levelValue) / 100 : 0;
                    break;
                case 3:
                    duty = Math.floor((sample * 3 + 127) / 255) * 85;
                    duty = (duty * levelValue) / 100;
                    break;
                default:
                    duty = (sample * levelValue) / 100;
                    break;
            }
            return Math.max(0, Math.min(255, duty));
        }

        function drawTransmitPreview(samples, endSample = samples.length) {
            const width = canvas.width = canvas.offsetWidth;
            const height = canvas.height = canvas.offsetHeight;
            canvasCtx.fillStyle = '#222';
            canvasCtx.fillRect(0, 0, width, height);
            canvasCtx.strokeStyle = '#00ff00';
            canvasCtx.lineWidth = 2;
            canvasCtx.beginPath();
            const sampleRate = 16000;
            const displayedSamples = Math.min(samples.length,
                Math.max(1, Math.floor(sampleRate * Number(timeScale.value) / 1000)));
            const windowStart = Math.max(0, endSample - displayedSamples);
            const windowLength = Math.max(1, endSample - windowStart);
            for (let x = 0; x < width; x++) {
                const sampleIndex = Math.min(
                    windowStart + Math.floor((x / Math.max(1, width - 1)) * (windowLength - 1)),
                    samples.length - 1
                );
                const sample = samples[sampleIndex];
                const y = height / 2 - sample * height * 0.45;
                if (x === 0) canvasCtx.moveTo(x, y);
                else canvasCtx.lineTo(x, y);
            }
            canvasCtx.stroke();

            const pinWidth = pinCanvas.width = pinCanvas.offsetWidth;
            const pinHeight = pinCanvas.height = pinCanvas.offsetHeight;
            pinCanvasCtx.fillStyle = '#222';
            pinCanvasCtx.fillRect(0, 0, pinWidth, pinHeight);
            pinCanvasCtx.strokeStyle = '#ffcc00';
            pinCanvasCtx.lineWidth = 2;
            pinCanvasCtx.beginPath();
            for (let x = 0; x < pinWidth; x++) {
                const sampleIndex = Math.min(
                    windowStart + Math.floor((x / Math.max(1, pinWidth - 1)) * (windowLength - 1)),
                    samples.length - 1
                );
                const sample = Math.round((samples[sampleIndex] + 1) * 127.5);
                const duty = modulationDuty(sample) / 255;
                const phase = (x / pinWidth * Number(timeScale.value) / 1000 * 15000) % 1;
                const high = phase < duty;
                const y = high ? pinHeight * 0.25 : pinHeight * 0.75;
                if (x === 0) pinCanvasCtx.moveTo(x, y);
                else pinCanvasCtx.lineTo(x, y);
            }
            pinCanvasCtx.stroke();
        }

        async function updateConfig() {
            levelValue.value = level.value + '%';
            volumeValue.value = volume.value + '%';
            try {
                const response = await fetch('/config?level=' + level.value +
                    '&mode=' + modulation.value + '&volume=' + volume.value);
                if (!response.ok) throw new Error(await response.text() || 'Configuration rejected: HTTP ' + response.status);
                if (previewSamples) drawTransmitPreview(previewSamples, previewPosition);
                return true;
            } catch (err) {
                setStatus('Configuration error: ' + err.message, 'error');
                return false;
            }
        }

        level.addEventListener('input', updateConfig);
        volume.addEventListener('input', () => {
            if (monitorGain) monitorGain.gain.setValueAtTime(Number(volume.value) / 100, audioCtx.currentTime);
            updateConfig();
        });
        document.getElementById('monitorEnabled').addEventListener('change', () => {
            if (!document.getElementById('monitorEnabled').checked) stopMonitor();
        });
        modulation.addEventListener('change', updateConfig);
        timeScale.addEventListener('input', () => {
            timeScaleValue.value = timeScale.value + ' ms';
            if (previewSamples) drawTransmitPreview(previewSamples, previewPosition);
        });

        async function sendSamples(samples) {
            if (!samples.length || samples.length > 128000) throw new Error('Audio must be 8 seconds or shorter');
            const token = operation;
            previewSamples = samples;
            previewPosition = samples.length;
            drawTransmitPreview(samples);
            for (let offset = 0; offset < samples.length; offset += 1024) {
                if (operation !== token) throw new Error('Upload cancelled');
                const end = Math.min(offset + 1024, samples.length);
                const chunk = new Uint8Array(end - offset);
                for (let i = offset; i < end; i++) {
                    chunk[i - offset] = Math.max(0, Math.min(255, Math.round((samples[i] + 1) * 127.5)));
                }
                const response = await fetch('/clip?offset=' + offset + '&total=' + samples.length,
                    {method: 'POST', headers: {'Content-Type': 'application/octet-stream'}, body: chunk});
                if (!response.ok) throw new Error(await response.text());
                setStatus('Uploading: ' + Math.round(end / samples.length * 100) + '%');
            }
            if (operation !== token) throw new Error('Upload cancelled');
            document.getElementById('fileInfo').textContent =
                (samples.length / 16000).toFixed(2) + ' seconds uploaded (' + samples.length + ' bytes). Ready to replay.';
            setStatus('Upload complete. Press Play uploaded audio.');
        }

        function makeTone(frequency, seconds) {
            const sampleRate = 16000;
            const samples = new Float32Array(sampleRate * seconds);
            for (let i = 0; i < samples.length; i++) {
                samples[i] = Math.sin(2 * Math.PI * frequency * i / sampleRate) * 0.8;
            }
            return samples;
        }

        function resample(samples, inputRate, outputRate) {
            if (inputRate === outputRate) return samples;
            const outputLength = Math.floor(samples.length * outputRate / inputRate);
            const output = new Float32Array(outputLength);
            for (let i = 0; i < output.length; i++) {
                const position = i * inputRate / outputRate;
                const left = Math.floor(position);
                const right = Math.min(left + 1, samples.length - 1);
                const fraction = position - left;
                output[i] = samples[left] * (1 - fraction) + samples[right] * fraction;
            }
            return output;
        }

        async function uploadSource(makeSamples) {
            if (uploadBusy) return;
            uploadBusy = true;
            try {
                if (!await controlOutput('off')) return;
                const token = operation;
                const samples = await makeSamples();
                if (operation !== token) throw new Error('Upload cancelled');
                await sendSamples(samples);
            } catch (err) { setStatus(err.message, 'error'); }
            finally { uploadBusy = false; }
        }
        document.getElementById('send440').onclick = () => uploadSource(() => makeTone(440, 3));
        document.getElementById('send1000').onclick = () => uploadSource(() => makeTone(1000, 3));
        document.getElementById('sendFile').onclick = () => uploadSource(async () => {
            const file = document.getElementById('audioFile').files[0];
            if (!file) throw new Error('Choose an audio file first');
            if (file.size > 10 * 1024 * 1024) throw new Error('Source file limit is 10 MiB; decoded audio limit is 8 seconds');
            setStatus('Decoding audio...');
            audioCtx = audioCtx || new (window.AudioContext || window.webkitAudioContext)();
            const decoded = await audioCtx.decodeAudioData(await file.arrayBuffer());
            if (decoded.duration > 8) throw new Error('Trim the audio to 8 seconds or less');
            const mono = new Float32Array(decoded.length);
            for (let channel = 0; channel < decoded.numberOfChannels; channel++) {
                const data = decoded.getChannelData(channel);
                for (let i = 0; i < mono.length; i++) mono[i] += data[i] / decoded.numberOfChannels;
            }
            return resample(mono, decoded.sampleRate, 16000);
        });
        document.getElementById('playClip').onclick = async () => {
            if (uploadBusy || preparingPlayback) { setStatus('Wait for the current operation to finish'); return; }
            preparingPlayback = true;
            try {
                // Resume in the click gesture, before waiting for any network requests.
                audioCtx = audioCtx || new (window.AudioContext || window.webkitAudioContext)();
                const resumed = audioCtx.resume();
                const stopped = controlOutput('off');
                const token = operation;
                await resumed;
                if (!await stopped || operation !== token) return;
                monitorBuffer = null;
                if (document.getElementById('monitorEnabled').checked) {
                    const response = await fetch('/clip', {cache: 'no-store'});
                    if (!response.ok) throw new Error(await response.text());
                    const pcm = new Uint8Array(await response.arrayBuffer());
                    if (!pcm.length || pcm.length > 128000) throw new Error('Invalid stored audio');
                    monitorBuffer = audioCtx.createBuffer(1, pcm.length, 16000);
                    const channel = monitorBuffer.getChannelData(0);
                    for (let i = 0; i < pcm.length; i++) channel[i] = (pcm[i] - 128) / 128;
                }
                if (operation !== token) return;
                if (!await updateConfig()) return;
                if (operation !== token) return;
                await controlOutput('audio');
            } catch (err) {
                stopMonitor();
                setStatus('Playback error: ' + err.message, 'error');
            } finally { preparingPlayback = false; }
        };
    </script>
</body>
</html>
)rawliteral";

void handleRoot() {
    server.send(200, "text/html", PAGE_HTML);
}

void handleConfig() {
    if (server.hasArg("volume")) {
        audioVolume = static_cast<uint8_t>(constrain(server.arg("volume").toInt(), 0, 100));
    }
    if (server.hasArg("level")) {
        outputLevel = static_cast<uint8_t>(constrain(server.arg("level").toInt(), 0, 100));
    }
    if (server.hasArg("mode")) {
        modulationMode = static_cast<uint8_t>(constrain(server.arg("mode").toInt(), 0, 3));
    }
    server.send(200, "text/plain", "Configuration updated");
}

uint8_t applyModulation(uint8_t sample) {
    sample = static_cast<uint8_t>(128 + (static_cast<int>(sample) - 128) * audioVolume / 100);
    int duty;
    switch (modulationMode) {
        case 1:
            duty = (128 + ((static_cast<int>(sample) - 128) * outputLevel) / 100)
                * outputLevel / 100;
            break;
        case 2:
            duty = sample > 128 ? (255 * outputLevel) / 100 : 0;
            break;
        case 3:
            duty = ((static_cast<int>(sample) * 3 + 127) / 255) * 85;
            duty = (duty * outputLevel) / 100;
            break;
        default:
            duty = (sample * outputLevel) / 100;
            break;
    }
    return static_cast<uint8_t>(constrain(duty, 0, 255));
}

void handleClipDownload() {
    if (!clipLength) {
        server.send(409, "text/plain", "Upload a complete audio clip first");
        return;
    }
    if (outputMode != OUTPUT_OFF) {
        server.send(409, "text/plain", "Stop output before fetching audio");
        return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(clipLength);
    server.send(200, "application/octet-stream", "");
    server.sendContent(reinterpret_cast<const char*>(clipBuffer), clipLength);
}

void handleClip() {
    if (outputMode != OUTPUT_OFF) {
        server.send(409, "text/plain", "Stop output before uploading");
        return;
    }
    const long offset = server.arg("offset").toInt();
    const long total = server.arg("total").toInt();
    const String body = server.arg("plain");
    if (!server.hasArg("offset") || total <= 0 || total > MAX_CLIP_BYTES ||
        offset < 0 || body.length() == 0 || body.length() > 1024 ||
        static_cast<size_t>(offset) + body.length() > static_cast<size_t>(total)) {
        server.send(400, "text/plain", "Invalid audio chunk or clip exceeds 8 seconds");
        return;
    }
    if (offset == 0) {
        clipLength = 0;
        uploadLength = 0;
        uploadExpected = total;
    }
    if (static_cast<size_t>(offset) != uploadLength || static_cast<size_t>(total) != uploadExpected) {
        server.send(409, "text/plain", "Upload out of sequence; upload again");
        return;
    }
    memcpy(clipBuffer + offset, body.c_str(), body.length());
    uploadLength += body.length();
    if (uploadLength == uploadExpected) clipLength = uploadLength;
    server.send(200, "text/plain", "Audio chunk stored");
}

void stopOutput() {
    clipPlaying = false;
    if (audioTimer) {
        esp_timer_stop(audioTimer);
    }
    audioFinished = false;
    if (testPwmPins) {
        ledcWriteChannel(PWM_CHANNEL, 0);
        for (uint8_t i = 0; i < testPwmPins; ++i) {
            ledcDetach(DIGITAL_PINS[i]);
            pinMode(DIGITAL_PINS[i], OUTPUT);
            digitalWrite(DIGITAL_PINS[i], LOW);
        }
        testPwmPins = 0;
    }
    if (outputMode == OUTPUT_TEST) {
        for (uint8_t pin : DIGITAL_PINS) {
            digitalWrite(pin, LOW);
        }
    }
    outputMode = OUTPUT_OFF;
    if (pwmAttached) {
        ledcWrite(LASER_PIN, 0);
        ledcDetach(LASER_PIN);
        pwmAttached = false;
    }
    digitalWrite(LASER_PIN, LOW);
    pinMode(LASER_PIN, OUTPUT);
    digitalWrite(LASER_PIN, LOW);
}

void audioSampleTick(void*) {
    if (!clipPlaying || outputMode != OUTPUT_AUDIO || !clipLength) return;
    if (audioSampleIndex >= clipLength) {
        if (clipRepeat) {
            audioSampleIndex = 0;
        } else {
            clipPlaying = false;
            audioFinished = true;
            ledcWrite(LASER_PIN, 0);
            return;
        }
    }
    ledcWrite(LASER_PIN, applyModulation(clipBuffer[audioSampleIndex++]));
}

void handleOutput() {
    const String pinText = server.arg("pin");
    int pinIndex = -1;
    for (int i = 0; i < 11; ++i) {
        if (pinText == String(i)) pinIndex = i;
    }
    const String mode = server.arg("mode");
    if (pinIndex < 0 || (mode != "off" && mode != "test" && mode != "audio")) {
        server.send(400, "text/plain", "Invalid pin or output mode");
        return;
    }
    double requestedHz = testHz;
    if (mode == "test") {
        const String hzText = server.arg("hz");
        char* end = nullptr;
        requestedHz = strtod(hzText.c_str(), &end);
        if (end == hzText.c_str() || *end != '\0' || !isfinite(requestedHz) ||
            requestedHz < 0.1 || requestedHz > 15000) {
            server.send(400, "text/plain", "Frequency must be 0.1 to 15000 Hz");
            return;
        }
    }
    stopOutput();  // Old pin remains LOW when switching.
    selectedPin = static_cast<uint8_t>(pinIndex);
    stopOutput();
    if (mode == "test") {
        for (uint8_t pin : DIGITAL_PINS) {
            digitalWrite(pin, LOW);
            pinMode(pin, OUTPUT);
            digitalWrite(pin, LOW);
        }
        testHz = requestedHz;
        outputMode = OUTPUT_TEST;
        if (testHz >= 100) {
            // One hardware PWM channel feeds all pins, avoiding WiFi timing jitter.
            const uint32_t frequency = static_cast<uint32_t>(round(testHz));
            for (uint8_t pin : DIGITAL_PINS) {
                if (!ledcAttachChannel(pin, frequency, 10, PWM_CHANNEL)) {
                    stopOutput();
                    server.send(500, "text/plain", "Test PWM setup failed; outputs LOW");
                    return;
                }
                ++testPwmPins;
            }
            ledcWriteChannel(PWM_CHANNEL, 512);
            testHz = ledcReadFreq(DIGITAL_PINS[0]);
        } else {
            testPeriodUs = static_cast<uint32_t>(round(1000000.0 / testHz));
            testStarted = micros();
        }
    } else if (mode == "audio") {
        if (!clipLength) {
            server.send(409, "text/plain", "Upload a complete audio clip first");
            return;
        }
        if (!audioTimer) {
            server.send(500, "text/plain", "Audio timer is unavailable; output LOW");
            return;
        }
        pwmAttached = ledcAttach(LASER_PIN, PWM_FREQ, PWM_RES);
        if (!pwmAttached) {
            stopOutput();
            server.send(500, "text/plain", "PWM setup failed; output LOW");
            return;
        }
        ledcWrite(LASER_PIN, 0);
        outputMode = OUTPUT_AUDIO;
        clipRepeat = server.arg("repeat") == "1";
        audioSampleIndex = 0;
        audioFinished = false;
        clipPlaying = true;
        if (esp_timer_start_periodic(audioTimer, 62) != ESP_OK) {
            stopOutput();
            server.send(500, "text/plain", "Audio timer setup failed; output LOW");
            return;
        }
    }
    if (mode == "test") {
        server.send(200, "text/plain", "D0-D10: continuous test at " + String(testHz, 3) + " Hz");
        return;
    }
    server.send(200, "text/plain", "D" + String(selectedPin) + " / GPIO" +
        String(LASER_PIN) + ": " + (mode == "test" ? "continuous 1 Hz test" :
        mode == "audio" ? (clipRepeat ? "playing uploaded audio on repeat until Stop" : "playing uploaded audio once") : "stopped, output LOW"));
}

void setup() {
    stopOutput();  // Earliest sketch initialization; cannot control ROM boot state.
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\nESP32-C3 Laser Audio Transmitter");
    Serial.println("================================");

    // Setup PWM for laser

    Serial.println("Laser output initialized LOW on GPIO" + String(LASER_PIN));

    // Connect to WiFi
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi Connected!");
        Serial.println("IP Address: " + WiFi.localIP().toString());
    } else {
        Serial.println("\nFailed to connect to WiFi!");
    }

    // Setup web server
    server.on("/", HTTP_GET, handleRoot);
    server.on("/output", HTTP_POST, handleOutput);
    server.on("/config", HTTP_GET, handleConfig);
    server.on("/clip", HTTP_GET, handleClipDownload);
    server.on("/clip", HTTP_POST, handleClip);
    server.begin();
    const esp_timer_create_args_t audioTimerArgs = {
        .callback = &audioSampleTick,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_sample"
    };
    if (esp_timer_create(&audioTimerArgs, &audioTimer) != ESP_OK) {
        Serial.println("Audio timer creation failed");
    }
    Serial.println("Web server started on http://" + WiFi.localIP().toString());
}

void loop() {
    server.handleClient();
    if (audioFinished) {
        stopOutput();
    }
    if (outputMode == OUTPUT_TEST && testPwmPins == 0) {
        // Unsigned subtraction remains valid across micros() rollover. Low-frequency timing can be delayed by HTTP handling.
        const uint8_t level = ((micros() - testStarted) % testPeriodUs) < testPeriodUs / 2 ? HIGH : LOW;
        for (uint8_t pin : DIGITAL_PINS) {
            digitalWrite(pin, level);
        }
    }
}
