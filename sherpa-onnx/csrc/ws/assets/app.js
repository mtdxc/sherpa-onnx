
class Mp3Encode {
    constructor(sampleRate = 44100, bitRate = 128) {
        this.encoder = new lamejs.Mp3Encoder(1, sampleRate, bitRate);
        this.sampleBlockSize = 1152;
        this.pos = 0;
        this.pcm = new Int16Array(this.sampleBlockSize);
    }

    encodeShort(buffer) {
        let mp3Data = [];
        for (let i = 0; i < buffer.length; i++) {
            this.pcm[this.pos++] = buffer[i];
            if (this.pos == this.sampleBlockSize) {
                const mp3buf = this.encoder.encodeBuffer(this.pcm);
                if (mp3buf.length > 0) {
                    mp3Data.push(mp3buf);
                }
                this.pos = 0;
            }
        }
        return mp3Data;
    }

    encodeFloat(buffer) {
        let mp3Data = [];
        for (let i = 0; i < buffer.length; i++) {
            this.pcm[this.pos++] = buffer[i] * 32768;
            if (this.pos == this.sampleBlockSize) {
                const mp3buf = this.encoder.encodeBuffer(this.pcm);
                if (mp3buf.length > 0) {
                    mp3Data.push(mp3buf);
                }
                this.pos = 0;
            }
        }
        return mp3Data;
    }

    flush() {
        const mp3buf = this.encoder.flush();
        if (mp3buf.length > 0) {
            return mp3buf;
        }
        return null;
    }
}

const demoapp = {
    text: '讲个冷笑话吧，要很好笑的那种。',
    llm_input: 'hello world',
    recording: false,
    asrWS: null,
    currentText: null,
    disabled: false,
    useMp3: false,
    elapsedTime: null,
    logs: [{ idx: 0, text: 'Happily here at ruzhila.cn.', my: true }],
    async init() {
    },
    
    audioElement:null,
    playBuffer:null,
    startPlay() {
      this.audioElement= document.getElementById('audio');
      // 将 MediaSource 绑定到 audio 标签
      const mediaSource = new MediaSource();
      this.audioElement.src = URL.createObjectURL(mediaSource);
      mediaSource.addEventListener('sourceopen', () => {
        this.playBuffer = mediaSource.addSourceBuffer('audio/mpeg');
      });
    },
    stopPlay() {
      if (this.audioElement) {
        this.audioElement.src = "";
      }
      this.playBuffer = null;
      /* 这会有个停止延迟，采用后者可立马停止
      mediaSource.removeSourceBuffer(this.playBuffer);
      this.playBuffer = mediaSource.addSourceBuffer('audio/mpeg');
      */
    },
    breakPlayout() {
      if (this.playBuffer) {
        this.stopPlay();
        this.startPlay();
      }
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
        this.stopPlay();
        this.currentText = null;

    },

    async doasr() {
        const audioConstraints = {
            video: false,
            audio: true,
        };

        const mediaStream = await navigator.mediaDevices.getUserMedia(audioConstraints);
        let mp3Enc = null;
        if (this.useMp3) {
            mp3Enc = new Mp3Encode(16000, 64);
            this.startPlay();
        }

        const ws = new WebSocket(this.useMp3 ? '/asrmp3' : '/asr');
        let currentMessage = '';

        ws.onopen = () => {
            this.logs = [];
        };
        ws.onclose = () => {
            this.stopasr();
        };
        ws.onerror = (e) => {
            console.error('WebSocket error:', e);
            this.stopasr();
        };
        
        let audioContext = new AudioContext({ sampleRate: 16000 })
        await audioContext.audioWorklet.addModule('./audio_process.js')
        const playNode = new AudioWorkletNode(audioContext, 'play-audio-processor');
        playNode.connect(audioContext.destination);
        ws.onmessage = (e) => {
            if (e.data instanceof Blob) {
                e.data.arrayBuffer().then(async (arrayBuffer) => {
                    if (this.playBuffer) {
                        // 播放延迟相对延迟高点
                        this.playBuffer.appendBuffer(arrayBuffer);
                        return;
                        // 这易卡顿，且要求一次至少解码2帧数据，否则会解码失败
                        const audioBuffer = await audioContext.decodeAudioData(arrayBuffer);
                        playNode.port.postMessage({ message: 'audioData', audioData: audioBuffer.getChannelData(0) });
                    } else {
                        const int16Array = new Int16Array(arrayBuffer);
                        let float32Array = new Float32Array(int16Array.length);
                        for (let i = 0; i < int16Array.length; i++) {
                            float32Array[i] = int16Array[i] / 32768.;
                        }
                        playNode.port.postMessage({ message: 'audioData', audioData: float32Array });
                    }
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
                if (mp3Enc) {
                    const buff = mp3Enc.encodeShort(int16Array);
                    if (buff.length > 0) {
                        // 将编码后的数据发送到服务器
                        ws.send(new Blob(buff, { type: 'audio/mpeg' }));
                    }
                }
                else{
                    ws.send(int16Array.buffer);
                }
            }
        }
        
        const source = audioContext.createMediaStreamSource(mediaStream);
        source.connect(recordNode);
        this.asrWS = ws;
        this.recording = true;
    }
}
