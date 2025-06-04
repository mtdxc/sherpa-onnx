// sherpa-onnx/csrc/online-websocket-server-impl.cc
//
// Copyright (c)  2022-2023  Xiaomi Corporation

#include "sherpa-websocket-server-impl.h"

#include <vector>
#include "sherpa-onnx/csrc/file-utils.h"
#include "sherpa-onnx/csrc/log.h"

namespace sherpa_onnx {

void WebsocketServerConfig::Register(sherpa_onnx::ParseOptions *po) {
  online_config.Register(po);
  offline_config.Register(po);
  tts_config.Register(po);
  vad_config.Register(po);
  po->Register("log-file", &log_file,
               "Path to the log file. Logs are "
               "appended to this file");
}

void WebsocketServerConfig::Validate() const {
  online_config.Validate();
  tts_config.Validate();
  vad_config.Validate();
  offline_config.Validate();
}

SherpaWebsocketServer::SherpaWebsocketServer(
    hv::EventLoopThreadPool* io_work,
    const WebsocketServerConfig &config)
    : config_(config),
      io_work_(io_work) {
  this->asr_online_ = std::make_unique<OnlineRecognizer>(config.online_config);
  this->tts_ = std::make_unique<OfflineTts>(config.tts_config);
  //onclose = [this](const WebSocketChannelPtr & ch) {OnClose(ch); };
  onopen = [this](const WebSocketChannelPtr &ch, const HttpRequestPtr & req) {
    OnOpen(ch, req);
  };
  onmessage = [this](const WebSocketChannelPtr &ch, const std::string &msg) {
    OnMessage(ch, msg);
  };
}

void SherpaWebsocketServer::Run(uint16_t port) {
  //server_.set_reuse_addr(true);
  //server_.listen(asio::ip::tcp::v4(), port);
  //server_.start_accept();
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
    ret->s = asr_online_->CreateStream();
  else if(asr_offline_) {
    ret->os = asr_offline_->CreateStream();
    ret->vad_ = std::make_unique<VoiceActivityDetector>(config_.vad_config);
  }
  else {
    SHERPA_ONNX_LOGE("No ASR model is loaded!");
    return;
  }
  ret->worker_ = io_work_->loop().get();
  hdl->setContextPtr(ret);
  SHERPA_ONNX_LOG(INFO) << "New connection: " << hdl->peeraddr();
}

void SherpaWebsocketServer::OnMessage(connection_hdl hdl, const std::string &payload) {
  auto c = hdl->getContextPtr<Connection>();
  switch (hdl->opcode) {
    case WS_OPCODE_TEXT:
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
    case eByte:
      break;
  }
  if (auto s = c->s.get()) {
    if (samples.size()) {
      s->AcceptWaveform(c->samplerate, samples.data(), samples.size());
    }
    else{
      samples.resize(c->samplerate * 0.1); // 100ms silence
      s->AcceptWaveform(c->samplerate, samples.data(), samples.size());
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
  } else {
    /* Offline ASR
    if (c->vad_) {
      c->vad_->AcceptWaveform(samples.data(), samples.size());
      if (c->vad_->IsSpeech()) {
        c->os->AcceptWaveform(c->vad_->GetSampleRate(), samples.data(), samples.size());
      }
    } else {
      c->os->AcceptWaveform(c->s->GetSampleRate(), samples.data(), samples.size());
    }*/
  }
}

void SherpaWebsocketServer::doTts(connection_hdl hdl, const std::string &msg) {
  auto c = hdl->getContextPtr<Connection>();
  int index = c->tts_index_;
  tts_->Generate(msg, 0, 1.0f,
      [hdl, c, index](const float *samples, int32_t n, float progress) {
        if (hdl->isClosed() || c->tts_index_ != index) return 0;
        std::string data;
        data.assign((const char*)samples, n * sizeof(float));
        c->tts_lines_.push_back(data);
        return 1;
      });
  c->tts_line_.empty();
}

void Connection::onAsrLine(std::string line){
  tts_index_++;
}

bool Connection::popTtsFrame(std::string &frame) {
  if (!tts_wavs_.empty()) {
    frame = tts_wavs_.front();
    tts_wavs_.pop_front();
  }
  if (tts_line_.empty() && tts_wavs_.size() < 5) {
    // ������һ��
    tts_line_ = tts_lines_.front();
    tts_lines_.pop_front();
    auto self = shared_from_this();
    auto index = tts_index_;
    worker_->runInLoop([self, index]() {
      //SherpaWebsocketServer::doTts(self->hdl, self->tts_line_);
    });
  }
  return frame.length();
}

}  // namespace sherpa_onnx
