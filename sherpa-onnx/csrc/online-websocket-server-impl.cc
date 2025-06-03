// sherpa-onnx/csrc/online-websocket-server-impl.cc
//
// Copyright (c)  2022-2023  Xiaomi Corporation

#include "sherpa-onnx/csrc/online-websocket-server-impl.h"

#include <vector>

#include "sherpa-onnx/csrc/file-utils.h"
#include "sherpa-onnx/csrc/log.h"

namespace sherpa_onnx {

void OnlineWebsocketDecoderConfig::Register(ParseOptions *po) {
  recognizer_config.Register(po);

  po->Register("loop-interval-ms", &loop_interval_ms,
               "It determines how often the decoder loop runs. ");

  po->Register("max-batch-size", &max_batch_size,
               "Max batch size for recognition.");

  po->Register("end-tail-padding", &end_tail_padding,
               "It determines the length of tail_padding at the end of audio.");
}

void OnlineWebsocketDecoderConfig::Validate() const {
  recognizer_config.Validate();
  SHERPA_ONNX_CHECK_GT(loop_interval_ms, 0);
  SHERPA_ONNX_CHECK_GT(max_batch_size, 0);
  SHERPA_ONNX_CHECK_GT(end_tail_padding, 0);
}

void OnlineWebsocketServerConfig::Register(sherpa_onnx::ParseOptions *po) {
  decoder_config.Register(po);

  po->Register("log-file", &log_file,
               "Path to the log file. Logs are "
               "appended to this file");
}

void OnlineWebsocketServerConfig::Validate() const {
  decoder_config.Validate();
}

OnlineWebsocketDecoder::OnlineWebsocketDecoder(OnlineWebsocketServer *server)
    : server_(server),
      config_(server->GetConfig().decoder_config) {
  recognizer_ = std::make_unique<OnlineRecognizer>(config_.recognizer_config);
}

std::shared_ptr<Connection> OnlineWebsocketDecoder::GetOrCreateConnection(WebSocketChannelPtr hdl) {
  auto ret = hdl->getContextPtr<Connection>();
  if (!ret) {
    ret = std::make_shared<Connection>(recognizer_->CreateStream());
    hdl->setContextPtr(ret);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  connections_.insert(hdl);
  return ret;
}

void OnlineWebsocketDecoder::AcceptWaveform(std::shared_ptr<Connection> c) {
  std::lock_guard<std::mutex> lock(c->mutex);
  float sample_rate = config_.recognizer_config.feat_config.sampling_rate;
  while (!c->samples.empty()) {
    const auto &s = c->samples.front();
    c->s->AcceptWaveform(sample_rate, s.data(), s.size());
    c->samples.pop_front();
  }
}

void OnlineWebsocketDecoder::InputFinished(std::shared_ptr<Connection> c) {
  std::lock_guard<std::mutex> lock(c->mutex);
  // flush remain data
  float sample_rate = config_.recognizer_config.feat_config.sampling_rate;
  while (!c->samples.empty()) {
    const auto &s = c->samples.front();
    c->s->AcceptWaveform(sample_rate, s.data(), s.size());
    c->samples.pop_front();
  }
  // fill end slience
  std::vector<float> tail_padding(config_.end_tail_padding * sample_rate);
  c->s->AcceptWaveform(sample_rate, tail_padding.data(), tail_padding.size());
  // mask input finished
  c->s->InputFinished();
  c->eof = true;
}

void OnlineWebsocketDecoder::Warmup() const {
  recognizer_->WarmpUpRecognizer(config_.recognizer_config.model_config.warm_up,
                                 config_.max_batch_size);
}

void OnlineWebsocketDecoder::Run() {
  if (timer_) return;
  timer_ = hv::setInterval(config_.loop_interval_ms,
                       [this](hv::TimerID id) { ProcessConnections(); });
}

void OnlineWebsocketDecoder::ProcessConnections() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<connection_hdl> to_remove;
  for (auto hdl : connections_) {
    // The order of `if` below matters!
    if (hdl->isClosed()) {
      // If the connection is disconnected, we stop processing it
      to_remove.push_back(hdl);
      continue;
    }

    if (active_.count(hdl)) {
      // Another thread is decoding this stream, so skip it
      continue;
    }

    auto c = hdl->getContextPtr<Connection>();
    if (!recognizer_->IsReady(c->s.get())) {
      if (c->eof) {
        // We won't receive samples from the client, so send a Done! to client
        hdl->send("Done!");
        to_remove.push_back(hdl);
      } else {
        // this stream has not enough frames to decode, so skip it
      }
      continue;
    }

    // TODO(fangun): If the connection is timed out, we need to also
    // add it to `to_remove`

    // this stream has enough frames and is currently not processed by any
    // threads, so put it into the ready queue
    ready_connections_.push_back(hdl);

    // In `Decode()`, it will remove hdl from `active_`
    active_.insert(hdl);
  }

  for (auto hdl : to_remove) {
    connections_.erase(hdl);
  }

  if (!ready_connections_.empty()) {
    server_->GetWorkContext()->loop()->runInLoop([this]() { Decode(); });
  }
}

void OnlineWebsocketDecoder::Decode() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (ready_connections_.empty()) {
    // There are no connections that are ready for decoding,
    // so we return directly
    return;
  }

  std::vector<connection_hdl> c_vec;
  std::vector<OnlineStream *> s_vec;
  while (!ready_connections_.empty() &&
         static_cast<int32_t>(s_vec.size()) < config_.max_batch_size) {
    auto hdl = ready_connections_.front();
    ready_connections_.pop_front();
    if (auto c = hdl->getContextPtr<Connection>()) {
      s_vec.push_back(c->s.get());
      c_vec.push_back(hdl);
    }
  }

  if (!ready_connections_.empty()) {
    // there are too many ready connections but this thread can only handle
    // max_batch_size connections at a time, so we schedule another call
    // to Decode() and let other threads to process the ready connections
    server_->GetWorkContext()->loop()->runInLoop([this]() { Decode(); });
  }

  lock.unlock();
  recognizer_->DecodeStreams(s_vec.data(), s_vec.size());
  lock.lock();

  for (int i = 0; i < s_vec.size();  i++) {
    auto s = s_vec[i];
    auto result = recognizer_->GetResult(s);
    if (recognizer_->IsEndpoint(s)) {
      result.is_final = true;
      printf("ep:%s\n", result.text.c_str());
      recognizer_->Reset(s);
    }
    auto c = c_vec[i]; 
    if (!recognizer_->IsReady(s) && c->getContextPtr<Connection>()->eof) {
      result.is_final = true;
      result.is_eof = true;
    }
    c->send(result.AsJsonString());
    active_.erase(c);
  }
}

OnlineWebsocketServer::OnlineWebsocketServer(
    hv::EventLoopThreadPool* io_work,
    const OnlineWebsocketServerConfig &config)
    : config_(config),
      io_work_(io_work),
      decoder_(this) {
  onclose = [this](const WebSocketChannelPtr & ch) {OnClose(ch); };
  onopen = [this](const WebSocketChannelPtr &ch, const HttpRequestPtr &) {
    OnOpen(ch);
  };
  onmessage = [this](const WebSocketChannelPtr &ch, const std::string &msg) {
    OnMessage(ch, msg);
  };
}

void OnlineWebsocketServer::Run(uint16_t port) {
  //server_.set_reuse_addr(true);
  //server_.listen(asio::ip::tcp::v4(), port);
  //server_.start_accept();
  auto recognizer_config = config_.decoder_config.recognizer_config;
  int32_t warm_up = recognizer_config.model_config.warm_up;
  const std::string &model_type = recognizer_config.model_config.model_type;
  if (0 < warm_up && warm_up < 100) {
    if (model_type == "zipformer2") {
      decoder_.Warmup();
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
  decoder_.Run();
}

void OnlineWebsocketServer::OnOpen(connection_hdl hdl) {
  std::lock_guard<std::mutex> lock(mutex_);
  connections_.insert(hdl);

  std::ostringstream os;
  os << "New connection: " << hdl->peeraddr()
     << "Number of active connections: " << connections_.size() << ".\n";
  SHERPA_ONNX_LOG(INFO) << os.str();
}

void OnlineWebsocketServer::OnClose(connection_hdl hdl) {
  std::lock_guard<std::mutex> lock(mutex_);
  connections_.erase(hdl);

  SHERPA_ONNX_LOG(INFO) << "Number of active connections: " << connections_.size() << "\n";
}

bool OnlineWebsocketServer::Contains(connection_hdl hdl) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return connections_.count(hdl);
}

void OnlineWebsocketServer::OnMessage(connection_hdl hdl, const std::string &payload) {
  auto c = decoder_.GetOrCreateConnection(hdl);

  switch (hdl->opcode) {
    case WS_OPCODE_TEXT:
      if (payload == "Done") {
        io_work_->loop()->runInLoop([this, c]() { decoder_.InputFinished(c); });
      }
      break;
    case WS_OPCODE_BINARY: {
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
      
      {
        std::lock_guard<std::mutex> lock(c->mutex);
        c->samples.push_back(std::move(samples));
      }

      io_work_->loop()->runInLoop([this, c]() { decoder_.AcceptWaveform(c); });
      break;
    }
    default:
      break;
  }
}


}  // namespace sherpa_onnx
