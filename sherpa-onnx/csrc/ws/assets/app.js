const demoapp = {
    text: '讲个冷笑话吧，要很好笑的那种。',
    llm_input: 'hello world',
    recording: false,
    asrWS: null,
    currentText: null,
    disabled: false,
    elapsedTime: null,
    logs: [{ idx: 0, text: 'Happily here at ruzhila.cn.', my: true }],
    async init() {
    },
    async talkWithLLM() {
        if (this.asrWS && this.llm_input.length > 0) {
            this.asrWS.send(JSON.stringify({ cmd: 'llm', msg: this.llm_input }));
            this.logs.push({ idx: this.logs.length + 1, text: this.llm_input, my: true });
        }
        this.llm_input = '';
    },
    async dotts() {
        let audioContext = new AudioContext({ sampleRate: 16000 })
        await audioContext.audioWorklet.addModule('./audio_process.js')

        const ws = new WebSocket('/tts');
        ws.onopen = () => {
            ws.send(JSON.stringify({cmd:'tts', msg:this.text}));
        };
        const playNode = new AudioWorkletNode(audioContext, 'play-audio-processor');
        playNode.connect(audioContext.destination);

        this.disabled = true;
        ws.onmessage = async (e) => {
            if (e.data instanceof Blob) {
                e.data.arrayBuffer().then((arrayBuffer) => {
                    const int16Array = new Int16Array(arrayBuffer);
                    let float32Array = new Float32Array(int16Array.length);
                    for (let i = 0; i < int16Array.length; i++) {
                        float32Array[i] = int16Array[i] / 32768.;
                    }
                    playNode.port.postMessage({ message: 'audioData', audioData: float32Array });
                });
            } else {
                this.elapsedTime = JSON.parse(e.data)?.elapsed;
                this.disabled = false;
            }
        }
    },

    async stopasr() {
        if (!this.asrWS) {
            return;
        }
        this.asrWS.close();
        this.asrWS = null;
        this.recording = false;
        if (this.currentText) {
            this.logs.push({ idx: this.logs.length + 1, text: this.currentText, my: true });
        }
        this.currentText = null;

    },

    async doasr() {
        const audioConstraints = {
            video: false,
            audio: true,
        };

        const mediaStream = await navigator.mediaDevices.getUserMedia(audioConstraints);

        const ws = new WebSocket('/asr');
        let currentMessage = '';

        ws.onopen = () => {
            this.logs = [];
        };

        let audioContext = new AudioContext({ sampleRate: 16000 })
        await audioContext.audioWorklet.addModule('./audio_process.js')
        const playNode = new AudioWorkletNode(audioContext, 'play-audio-processor');
        playNode.connect(audioContext.destination);
        ws.onmessage = (e) => {
            if (e.data instanceof Blob) {
                e.data.arrayBuffer().then((arrayBuffer) => {
                    const int16Array = new Int16Array(arrayBuffer);
                    let float32Array = new Float32Array(int16Array.length);
                    for (let i = 0; i < int16Array.length; i++) {
                        float32Array[i] = int16Array[i] / 32768.;
                    }
                    playNode.port.postMessage({ message: 'audioData', audioData: float32Array });
                });
            } else {
                const data = JSON.parse(e.data);
                if (data.cmd == "tts") {
                    const { text, idx } = data;
                    this.logs.push({ text: text, idx: idx, my: false });
                    return;
                }
                const { text, finished, idx } = data;
                currentMessage = text;
                this.currentText = text

                if (finished) {
                    this.logs.push({ text: currentMessage, idx: idx, my:true });
                    currentMessage = '';
                    this.currentText = null
                }
            }
        };

        audioContext = new AudioContext({ sampleRate: 16000 })
        await audioContext.audioWorklet.addModule('./audio_process.js')

        const recordNode = new AudioWorkletNode(audioContext, 'record-audio-processor');
        recordNode.connect(audioContext.destination);
        recordNode.port.onmessage = (event) => {
            if (ws && ws.readyState === WebSocket.OPEN) {
                const int16Array = event.data.data;
                ws.send(int16Array.buffer);
            }
        }
        const source = audioContext.createMediaStreamSource(mediaStream);
        source.connect(recordNode);
        this.asrWS = ws;
        this.recording = true;
    }
}
