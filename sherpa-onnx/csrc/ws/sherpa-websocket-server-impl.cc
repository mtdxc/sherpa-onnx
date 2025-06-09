// sherpa-onnx/csrc/online-websocket-server-impl.cc
//
// Copyright (c)  2022-2023  Xiaomi Corporation

#include "sherpa-websocket-server-impl.h"
#include "sherpa-onnx/csrc/text-utils.h"
#include "sherpa-onnx/csrc/file-utils.h"
#include "sherpa-onnx/csrc/log.h"
#include <vector>
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#include "hv/requests.h"
#include "hv/htime.h"

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
  //online_config.Register(po);
  offline_config.Register(po);
  tts_config.Register(po);
  vad_config.Register(po);
  po->Register("llm-url", &llm_url, "llm url to request");
  po->Register("llm-model", &llm_model, "llm model to request");
  po->Register("llm-key", &llm_key, "llm key to reqest");
  po->Register("llm-type", &llm_type, "llm type");
  po->Register("tts-frame-count", &tts_frame_count, "tts frame count");
  po->Register("tts-frame-size", &tts_frame_size, "tts frame size");
}

void WebsocketServerConfig::Validate() const {  
  online_config.Validate();
  offline_config.Validate();
  tts_config.Validate();
  vad_config.Validate();
}

SherpaWebsocketServer::SherpaWebsocketServer(hv::EventLoopThreadPool* io_work,
    const WebsocketServerConfig &config)
    : config_(config), io_work_(io_work) {
  if (config.offline_config.Validate() && config_.vad_config.Validate()) {
    hlogi("use offiline modle %s", config.offline_config.ToString().c_str());
    this->asr_offline_ = std::make_unique<OfflineRecognizer>(config.offline_config);
  }
  else if (config.online_config.Validate()) {
    hlogi("use online modle %s", config.online_config.ToString().c_str());
    this->asr_online_ = std::make_unique<OnlineRecognizer>(config.online_config);
  }
  if (config.tts_config.Validate()) {
    hlogi("use tts modle %s", config.tts_config.ToString().c_str());
    this->tts_ = std::make_unique<OfflineTts>(config.tts_config);
  }

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
  ret->out_frame_size = config_.tts_frame_size;
  if(asr_online_)
    ret->son = asr_online_->CreateStream();
  else if(asr_offline_) {
    ret->vad_ = std::make_unique<VoiceActivityDetector>(config_.vad_config);
  }
  else {
    SHERPA_ONNX_LOGE("No ASR model is loaded!");
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
            c->addReqIndex();
          } else if (cmd == "tts") {
            addTts(hdl, root["msg"].get<std::string>());
          } else if (cmd == "llm") {
            doLlm(hdl, root["msg"].get<std::string>());
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
      //hdl->send(result.AsJsonString());
      onAsrLine(hdl, result.text);
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
      //hdl->send(res.AsJsonString());
      onAsrLine(hdl, res.text);
      c->vad_->Pop();
    }
  }
}

void SherpaWebsocketServer::onAsrLine(connection_hdl hdl, const std::string& msg) {
  auto c = hdl->getContextPtr<Connection>();
  if (hv::trim(msg).empty()) {
    return ;
  }
  // 会触发打断
  int idx = c->addReqIndex();
  hlogi("onAsrLine %d %s", idx, msg.c_str());
  // 发送tts信息
  hv::Json j;
  j["cmd"] = "stt";
  j["text"] = msg;
  j["is_final"] = true;
  j["finished"] = true;
  j["idx"] = idx;
  hdl->send(j.dump(), WS_OPCODE_TEXT);

  // 打断TTS请求
  if(c->tts_lines_.size() > 0) {
    hlogi("clear %d tts lines", c->tts_lines_.size());
    c->tts_lines_.clear();
  }
  if (c->tts_line_.length()) {
    hlogi("cancel cur tts %s", c->tts_line_.c_str());
    c->tts_line_.clear();
  }

  // 打断llm请求
  if (c->llm_req_) {
    hlogi("cancel llm request");
    c->llm_req_->Cancel();
    c->llm_req_ = nullptr;
    c->llm_s_ = c->llm_f_ = 0;
  }
  c->llm_line_.clear();
  
  doLlm(hdl, msg);
}

void SherpaWebsocketServer::doLlm(connection_hdl hdl, const std::string &msg) {
  if (config_.llm_url.empty())
    return;
  auto c = hdl->getContextPtr<Connection>();
  if (msg.empty()) {
    return;
  }

  c->llm_ctx_["stream"] = true;
  c->llm_ctx_["model"] = config_.llm_model;
  hv::Json m;
  m["role"] = "user";
  m["content"] = msg;
  c->llm_ctx_["messages"].push_back(m);
  //c->llm_ctx_["prompt"] = msg;
  HttpRequestPtr req = std::make_shared<HttpRequest>();
  req->url = config_.llm_url;
  req->method = HTTP_POST;
  req->timeout = -1;  // 不超时
  req->SetHeader("Content-Type", "application/json");
  if (config_.llm_key.length()) {
    req->SetHeader("Authorization", "Bearer " + config_.llm_key);
  }
  req->SetBody(c->llm_ctx_.dump());

  c->llm_req_ = req;
  c->llm_s_ = gettick_ms();
  c->llm_f_ = 0;
  int idx = c->req_index_;
  hlogi("llm %d> start with prompt: %s", idx, msg.c_str());
  std::shared_ptr<bool> bstream = std::make_shared<bool>(false);
  req->http_cb = [req, bstream, hdl, c, idx, this](
                     HttpMessage *resp, http_parser_state state,
                     const char *data, size_t size) {
    if (state == HP_HEADERS_COMPLETE) {
      if (resp->headers["Content-Type"] == "text/event-stream") {
        *bstream = true;
      }
    } else if (state == HP_BODY) {
      if (!hdl->isConnected() || c->req_index_ != idx) {
        hlogi("llm %d> break %d", idx, c->req_index_);
        req->Cancel();
        return;
      }
      /*binary body should check data*/
      // printf("%s", std::string(data, size).c_str());
      resp->body.append(data, size);
      if (!*bstream) {
        size_t ifind = std::string::npos;
        while ((ifind = resp->body.find("\n")) != std::string::npos) {
          std::string msg = resp->body.substr(0, ifind);
          // hlogi("%s", msg.c_str()); 
          try {
            // glm 返回 data: {} 导致json解析失败!
            auto pos = msg.find_first_of("{[");
            if (pos != 0 && pos != std::string::npos) 
              msg = msg.substr(pos);
            auto j = nlohmann::json::parse(msg);

            auto it = j.find("id");
            if (it != j.end()) {
              c->llm_ctx_["request_id"] = *it;
            }
            it = j.find("choices");
            if (it != j.end()) {
              // data: {"id":"202506091811223afeefd650714452","created":1749463882,"model":"glm-4-flash","choices":[{"index":0,"delta":{"role":"assistant","content":"Hello"}}]} 
              // print(chunk.choices[0].delta)
              for (auto ij : *it) {
                std::string text = ij["delta"]["content"];
                addTts(hdl, text, false);
              }
            }

            it = j.find("context");
            if (it != j.end()) {
              auto c = hdl->getContextPtr<Connection>();
              c->llm_ctx_["context"] = *it;
            }
            it = j.find("response");
            if (it != j.end()) {
              std::string text = *it;
              // @todo 转到 hdl的线程中执行 addTts
              addTts(hdl, text, j["done"]);
            }

            it = j.find("message");
            if (it != j.end()) {
              // {"model":"qwen3:latest","created_at":"2025-06-09T11:31:39.224166Z","message":{"role":"assistant","content":"\u003cthink\u003e"},"done":false}
              std::string text = it->at("content");
              addTts(hdl, text, j["done"]);
            }
          } catch (const std::exception &e) {
            // fprintf(stderr, "JSON parse error: %s\n", e.what());
          }
          resp->body.erase(0, ifind + 1);
        }
      } else {
        /*/n/n获取message*/
        size_t ifind = std::string::npos;
        while ((ifind = resp->body.find("\n\n")) != std::string::npos) {
          std::string msg = resp->body.substr(0, ifind + 2);
          resp->body.erase(0, ifind + 2);

          /*解析body,暂时不考虑多data
          id:xxx\n
          event:xxx\n
          data:xxx\n
          data:xxx\n
          data:xxx\n
          retry:10000\n
          */
          auto kvs = hv::splitKV(msg, '\n', ':');
          // if (!msg_cb(hv::Json(kvs))) req->Cancel();
        }
      }
    }
  };
  requests::async(req, [=](const HttpResponsePtr& resp){
    if (c->req_index_ == idx) {
      c->llm_req_ = nullptr;
      addTts(hdl, "", true);
      hlogi("llm %d> done, it tooks %d ms %d:%s", idx, gettick_ms() - c->llm_s_,
            resp->status_code, resp->body.c_str());
      c->llm_s_ = c->llm_f_ = 0;
    }
  });
}

void SherpaWebsocketServer::addTts(connection_hdl hdl, const std::string &msg, bool done) {
  auto c = hdl->getContextPtr<Connection>();
  if (c->tts_lines_.empty() && !c->tts_id_) {
    if (c->fmt == eByte && !c->mp3_enc_) {
      c->mp3_enc_ = mp3EncodeOpen(c->out_sample_rate, 1, 64000);
      c->out_frame_size = shine_samples_per_pass(c->mp3_enc_);
    }
    c->tts_id_ = hv::setInterval(c->out_frame_size * 1000 / c->out_sample_rate,
                        [=](hv::TimerID tId) { sendTtsFrame(hdl); });
  }
  if (!c->llm_f_ && c->llm_s_) {
    c->llm_f_ = gettick_ms();
    hlogi("llm %d> got first resp %s after %d ms", c->req_index_, msg.c_str(), c->llm_f_ - c->llm_s_);
  }

  c->llm_line_ += ToWideString(msg);
  static std::wstring space = L" \t\r\n";
  static std::wstring delim = L"。!！?？\n";
  // 加入断句处理
  while (c->llm_line_.length()) {
    size_t pos = c->llm_line_.find_first_of(delim);
    if (pos == -1) {
      break;
    }
    auto str = c->llm_line_.substr(0, pos + 1);
    if (-1 != str.find_first_not_of(space)) {
      c->tts_lines_.push_back(ToString(str));
    }
    c->llm_line_ = c->llm_line_.substr(pos + 1);
  }

  if (c->llm_line_.length() && done) {
    if (-1!= c->llm_line_.find_first_not_of(space)) {
      c->tts_lines_.push_back(ToString(c->llm_line_));
    }
    c->llm_line_.clear();
  }
  //c->tts_lines_.push_back(msg);
  doTts(hdl);
}

void SherpaWebsocketServer::sendTtsFrame(connection_hdl hdl) {
  auto c = hdl->getContextPtr<Connection>();
  if (!c->tts_wavs_.empty()) {
    auto frame = c->tts_wavs_.front();
    c->tts_wavs_.pop_front();
    hdl->send(frame, WS_OPCODE_BINARY);
  }
  doTts(hdl);
}

void SherpaWebsocketServer::doTts(connection_hdl hdl) {
  auto c = hdl->getContextPtr<Connection>();
  if (c->tts_line_.empty() && c->tts_lines_.size() &&
      c->tts_wavs_.size() < config_.tts_frame_count) {
    // 生成下一句
    std::string line = c->tts_lines_.front();
    c->tts_lines_.pop_front();
    io_work_->loop()->runInLoop([=]() { doTts(hdl, line); });
  }
}

void SherpaWebsocketServer::doTts(connection_hdl hdl, const std::string &msg) {
  auto c = hdl->getContextPtr<Connection>();
  int index = c->req_index_;
  hv::Json j;
  j["cmd"] = "tts";
  j["text"] = msg;
  j["idx"] = index;
  hdl->send(j.dump());

  if (!tts_) return;

  static std::string skip_tts = "<>{}";
  auto val = hv::trim(msg);
  if (val.empty()) {
    return ;
  } else if(skip_tts.find(*val.begin()) != std::string::npos 
    && skip_tts.find(*val.rbegin()) != std::string::npos) {
    hlogw("skip tts %s", val.c_str());
    return ;
  }
  hlogi("tts %d> start %s", index, msg.c_str());
  c->tts_line_ = msg;

  unsigned int f = 0, s = gettick_ms();
  int samplerate = tts_->SampleRate();
  auto res = tts_->Generate(msg, 0, 1.0f,
      [hdl, c, index, &f, s, samplerate](const float *samples, int32_t n, float progress) {
        if (!f) {
          f = gettick_ms();
          hlogi("tts %d> got first %d samples after %d ms, progress %f", index, n, f - s, progress);
        }
        if (hdl->isClosed() || c->req_index_ != index) {
          hlogi("tts %d> break for %d", index, c->req_index_);
          return 0; /// 打断
        }
        //printf("%d %f\n", n, progress);
        c->addTtsWav(samples, n, samplerate);
        return 1;
      });
  if (c->tts_line_ == msg) {
    int delta = gettick_ms() - s;
    hlogi("tts %d> %d text generate %5.2fs audio, tooks %d, %d ms", index, msg.length(),
          res.samples.size() * 1.0f / res.sample_rate, f - s, delta);
    c->tts_line_.clear();
  }
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
