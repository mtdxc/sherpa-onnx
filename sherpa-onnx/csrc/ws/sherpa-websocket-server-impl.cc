// sherpa-onnx/csrc/online-websocket-server-impl.cc
//
// Copyright (c)  2022-2023  Xiaomi Corporation

#include "sherpa-websocket-server-impl.h"

#include <vector>
#include "sherpa-onnx/csrc/file-utils.h"
#include "sherpa-onnx/csrc/log.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

namespace sherpa_onnx {
shine_t mp3EncodeOpen(int samplerate, int channel, int bitrate) {
  shine_config_t config;
  shine_set_config_mpeg_defaults(&config.mpeg);
  config.wave.samplerate = samplerate;
  if (channel > 1) {
    config.mpeg.mode = STEREO;
    config.wave.channels = PCM_STEREO;
  } else {
    config.mpeg.mode = MONO;
    config.wave.channels = PCM_MONO;
  }
  config.mpeg.bitr = bitrate / 1000;
  if (shine_check_config(config.wave.samplerate, config.mpeg.bitr) < 0) {
    std::cerr << "Unsupported samplerate/bitrate configuration.";
    return 0;
  }
  return shine_initialise(&config);
}
void WebsocketServerConfig::Register(sherpa_onnx::ParseOptions *po) {
  online_config.Register(po);
  offline_config.Register(po);
  tts_config.Register(po);
  vad_config.Register(po);
  po->Register("log-file", &log_file,
               "Path to the log file. Logs are appended to this file");
}

void WebsocketServerConfig::Validate() const {  
  online_config.Validate();
  //offline_config.Validate();
  tts_config.Validate();
  vad_config.Validate();
}

SherpaWebsocketServer::SherpaWebsocketServer(hv::EventLoopThreadPool* io_work,
    const WebsocketServerConfig &config)
    : config_(config), io_work_(io_work) {
  this->asr_online_ = std::make_unique<OnlineRecognizer>(config.online_config);
  if (config.offline_config.model_config.tokens.length()) {
    this->asr_offline_ = std::make_unique<OfflineRecognizer>(config.offline_config);
  }
  this->tts_ = std::make_unique<OfflineTts>(config.tts_config);
  onclose = [this](const WebSocketChannelPtr &ch) {
    SHERPA_ONNX_LOG(INFO) << "connection close: " << ch->peeraddr();
    if (auto c = ch->getContextPtr<Connection>()) {
      c->stop();
    }
  };
  onopen = [this](const WebSocketChannelPtr &ch, const HttpRequestPtr & req) {
    SHERPA_ONNX_LOG(INFO) << "New connection: " << ch->peeraddr();
    OnOpen(ch, req);
  };
  onmessage = [this](const WebSocketChannelPtr &ch, const std::string &msg) {
    OnMessage(ch, msg);
  };
  Run();
}

void SherpaWebsocketServer::Run() {
  auto recognizer_config = config_.online_config;
  int32_t warm_up = recognizer_config.model_config.warm_up;
  const std::string &model_type = recognizer_config.model_config.model_type;
  if (0 < warm_up && warm_up < 100) {
    if (model_type == "zipformer2") {
      asr_online_->WarmpUpRecognizer(recognizer_config.model_config.warm_up, 5);
      SHERPA_ONNX_LOGE("Warm up completed : %d times.", warm_up);
    } else {
      SHERPA_ONNX_LOGE("Only Zipformer2 has warmup support for now.");
      SHERPA_ONNX_LOGE("Given: %s", model_type.c_str());
      exit(0);
    }
  } else if (warm_up == 0) {
    SHERPA_ONNX_LOGE("Starting without warmup!");
  } else {
    SHERPA_ONNX_LOGE("Invalid Warm up Value!. Expected 0 < warm_up < 100");
    exit(0);
  }
}

void SherpaWebsocketServer::OnOpen(connection_hdl hdl, const HttpRequestPtr &req) {
  auto ret = std::make_shared<Connection>();
  if(asr_online_)
    ret->son = asr_online_->CreateStream();
  else if(asr_offline_) {
    ret->vad_ = std::make_unique<VoiceActivityDetector>(config_.vad_config);
  }
  else {
    SHERPA_ONNX_LOGE("No ASR model is loaded!");
    return;
  }
  ret->worker_ = io_work_->loop().get();
  hdl->setContextPtr(ret);
}

void SherpaWebsocketServer::OnMessage(connection_hdl hdl, const std::string &payload) {
  auto c = hdl->getContextPtr<Connection>();
  switch (hdl->opcode) {
    case WS_OPCODE_TEXT: 
        try{
          hv::Json root = hv::Json::parse(payload);
          std::string cmd = root["cmd"];
          if (cmd == "abort") {
            c->onAsrLine("");
          } else if (cmd == "tts") {
            addTts(hdl, root["msg"].get<std::string>());
          }
        } catch (std::exception e) {
          SHERPA_ONNX_LOGE("parse exception %s!", e.what());
        }
        break;
    case WS_OPCODE_BINARY: {
      c->worker_->runInLoop([this, hdl, payload]() {
        if (hdl->isClosed()) {
          return;
        }
        doAsr(hdl, payload);
      });
      break;
    }
    default:
      break;
  }
}

void SherpaWebsocketServer::doAsr(connection_hdl hdl, const std::string &payload) {
  auto c = hdl->getContextPtr<Connection>();
  std::vector<float> samples;
  switch (c->fmt){
    case eFloat: {
      auto p = reinterpret_cast<const float *>(payload.data());
      samples = std::vector<float>(p, p + payload.size() / sizeof(float));
      break;
    }
    case eShort: {
      auto p = reinterpret_cast<const short *>(payload.data());
      samples.resize(payload.size() / 2);
      for (int i = 0; i < samples.size(); i++) {
        samples[i] = *p++ / 32768.0f;
      }
      break;
    }
    case eByte: {
      drmp3_uint64 frames;
      drmp3_config cfg;
      float* pcm = drmp3_open_memory_and_read_pcm_frames_f32(payload.data(), payload.size(), &cfg, &frames, nullptr);
      if (pcm) {
        c->in_sample_rate = cfg.sampleRate;
        if (cfg.channels == 1) {
          samples = std::vector<float>(pcm, pcm + frames);
        }
        else {
          // 多通道变单通道
          samples.resize(frames);
          for (int i = 0; i < frames; i+=cfg.channels) {
            samples[i] = pcm[i];
          }
        }
        drmp3_free(pcm, nullptr);
      }
      break;
    }
  }

  if (auto s = c->son.get()) {
    if (samples.size()) {
      s->AcceptWaveform(c->in_sample_rate, samples.data(), samples.size());
    }
    else{
      samples.resize(c->in_sample_rate * 0.1); // 100ms silence
      s->AcceptWaveform(c->in_sample_rate, samples.data(), samples.size());
    }
    if(!asr_online_->IsReady(s)) {
      return ;
    }

    while (asr_online_->IsReady(s)) {
      asr_online_->DecodeStream(s);
    }
    auto result = asr_online_->GetResult(s);
    if (asr_online_->IsEndpoint(s)) {
      result.is_final = true;
      hdl->send(result.AsJsonString());
      c->onAsrLine(result.text);
      asr_online_->Reset(s);
    } else {
      // send intermediate results
      hdl->send(result.AsJsonString());
    }   
  } else if(c->vad_) {
    // Offline ASR
    c->vad_->AcceptWaveform(samples.data(), samples.size());
    while (!c->vad_->Empty()) {
      auto seg = c->vad_->Front();
      auto s = asr_offline_->CreateStream();
      s->AcceptWaveform(c->in_sample_rate, seg.samples.data(), seg.samples.size());
      asr_offline_->DecodeStream(s.get());
      auto res = s->GetResult();
      hdl->send(res.AsJsonString());
      c->onAsrLine(res.text);
      c->vad_->Pop();
    }
  }
}

void Connection::onAsrLine(std::string msg) {
  // 触发打断
  SHERPA_ONNX_LOGE("onAsrLine %s", msg.c_str());
  tts_index_++;
}

void SherpaWebsocketServer::addTts(connection_hdl hdl, const std::string &msg) {
  auto c = hdl->getContextPtr<Connection>();
  if (c->tts_lines_.empty() && !c->tts_id_) {
    if (c->fmt == eByte && !c->mp3_enc_) {
      c->mp3_enc_ = mp3EncodeOpen(c->out_sample_rate, 1, 64000);
      c->out_frame_size = shine_samples_per_pass(c->mp3_enc_);
    }
    c->tts_id_ = hv::setInterval(c->out_frame_size * 1000 / c->out_sample_rate,
                        [=](hv::TimerID tId) { sendTtsFrame(hdl); });
  }
  SHERPA_ONNX_LOGE("addTts %s", msg.c_str());
  c->tts_lines_.push_back(msg);
}

void SherpaWebsocketServer::sendTtsFrame(connection_hdl hdl) {
  auto c = hdl->getContextPtr<Connection>();
  if (!c->tts_wavs_.empty()) {
    auto frame = c->tts_wavs_.front();
    hdl->send(frame, WS_OPCODE_BINARY);
    c->tts_wavs_.pop_front();
  }
  if (c->tts_line_.empty() && c->tts_wavs_.size() < 5 && c->tts_lines_.size()) {
    // 提前生成下一句
    std::string line = c->tts_lines_.front();
    c->tts_lines_.pop_front();
    c->worker_->runInLoop([=]() { doTts(hdl, line); });
  }
}

void SherpaWebsocketServer::doTts(connection_hdl hdl, const std::string &msg) {
  auto c = hdl->getContextPtr<Connection>();
  c->tts_line_ = msg;
  SHERPA_ONNX_LOGE("doTts %s", msg.c_str());
  int index = c->tts_index_;
  int samplerate = tts_->SampleRate();
  tts_->Generate(msg, 0, 1.0f,
      [hdl, c, index, samplerate](const float *samples, int32_t n, float progress) {
        if (hdl->isClosed()) return 0; ///< 连接断开
        if (c->tts_index_ != index) return 0; /// 打断
        //printf("%d %f\n", n, progress);
        c->addTtsWav(samples, n, samplerate);
        return 1;
      });
  c->tts_line_.clear();
}

// 分帧
void Connection::addTtsWav(const float *data, int size, int samplerate) {
  if (!out_frame_size) return;
  std::vector<float> resampled;
  if (samplerate != out_sample_rate) {
    if (!resample_) {
      resample_ = std::make_unique<LinearResample>(samplerate, out_sample_rate);
    }
    // 需要采样率转换
    resample_->Resample(data, size, false, &resampled);
    data = resampled.data();
    size = resampled.size();
  }
  tts_cache_.insert(tts_cache_.end(), data, data + size);
  if (tts_cache_.size() < out_frame_size) return;
  int n = tts_cache_.size(), i = 0;
  while (n - i >= out_frame_size) {
    addTtsFrame(&tts_cache_[i], out_frame_size);
    i += out_frame_size;
  }
  if (i && n != i) {
    memmove(&tts_cache_[0], &tts_cache_[i], (n-i)*sizeof(float));
  }
  tts_cache_.resize(n-i);
}

void Connection::addTtsFrame(const float *data, int size) {
    std::vector<short> pcm;
    switch (fmt) {
    case eFloat:
      tts_wavs_.push_back(std::string((const char*)data, (const char*)(data+size)));
      break;
    case eShort: {
      pcm.resize(size);
      for (int i = 0; i < size; i++) {
        pcm[i] = data[i] * 32768;
      }
      tts_wavs_.push_back(
          std::string((const char *)pcm.data(), (const char *)(pcm.data()+size)));
      break;
    }
    case eByte: 
    if (mp3_enc_) {
      pcm.resize(size);
      for (int i = 0; i < size; i++) {
        pcm[i] = data[i] * 32768;
      }
      int len = 0;
      uint8_t* ret = shine_encode_buffer_interleaved(mp3_enc_, pcm.data(), &len);
      if (ret && len) tts_wavs_.push_back(std::string((const char*)ret, (const char*)(ret + len)));
    }
    break;
    default:
      break;
  }
}

void Connection::stop() {
  if (tts_id_) {
    hv::killTimer(tts_id_);
    tts_id_ = 0;
  }
  eof = true;
}

Connection::~Connection() {
  if (mp3_enc_) {
    shine_close(mp3_enc_);
    mp3_enc_ = nullptr;
  }
}

}  // namespace sherpa_onnx
