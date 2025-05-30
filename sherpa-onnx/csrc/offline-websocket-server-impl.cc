// sherpa-onnx/csrc/offline-websocket-server-impl.cc
//
// Copyright (c)  2022-2023  Xiaomi Corporation

#include "sherpa-onnx/csrc/offline-websocket-server-impl.h"

#include <algorithm>

#include "sherpa-onnx/csrc/macros.h"

namespace sherpa_onnx {

void OfflineWebsocketDecoderConfig::Register(ParseOptions *po) {
  recognizer_config.Register(po);

  po->Register("max-batch-size", &max_batch_size,
               "Max batch size for decoding.");

  po->Register("max-utterance-length", &max_utterance_length,
      "Max utterance length in seconds. If we receive an utterance "
      "longer than this value, we will reject the connection. "
      "If you have enough memory, you can select a large value for it.");
}

void OfflineWebsocketDecoderConfig::Validate() const {
  if (!recognizer_config.Validate()) {
    SHERPA_ONNX_LOGE("Error in recongizer config");
    exit(-1);
  }

  if (max_batch_size <= 0) {
    SHERPA_ONNX_LOGE("Expect --max-batch-size > 0. Given: %d", max_batch_size);
    exit(-1);
  }

  if (max_utterance_length <= 0) {
    SHERPA_ONNX_LOGE("Expect --max-utterance-length > 0. Given: %f", max_utterance_length);
    exit(-1);
  }
}

OfflineWebsocketDecoder::OfflineWebsocketDecoder(OfflineWebsocketServer *server)
    : server_(server),
      config_(server->GetConfig().decoder_config),
      recognizer_(config_.recognizer_config) {}

void OfflineWebsocketDecoder::Push(connection_hdl hdl, ConnectionDataPtr d) {
  std::lock_guard<std::mutex> lock(mutex_);
  streams_.push_back({hdl, d});
}

void OfflineWebsocketDecoder::Decode() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (streams_.empty()) {
    return;
  }

  int32_t size = std::min(static_cast<int32_t>(streams_.size()), config_.max_batch_size);
  SHERPA_ONNX_LOGE("size: %d", size);

  // We first lock the mutex for streams_, take items from it, and then
  // unlock the mutex; in doing so we don't need to lock the mutex to
  // access hdl and connection_data later.
  std::vector<connection_hdl> handles(size);

  std::vector<std::unique_ptr<OfflineStream>> ss(size);
  std::vector<OfflineStream *> p_ss(size);

  for (int32_t i = 0; i != size; ++i) {
    auto &p = streams_.front();
    handles[i] = p.first;
    auto c = p.second;
    streams_.pop_front();

    auto samples = reinterpret_cast<const float *>(&c->data[0]);
    auto num_samples = c->expected_byte_size / sizeof(float);
    auto s = recognizer_.CreateStream();
    s->AcceptWaveform(c->sample_rate, samples, num_samples);

    ss[i] = std::move(s);
    p_ss[i] = ss[i].get();
  }

  lock.unlock();

  // Note: DecodeStreams is thread-safe
  recognizer_.DecodeStreams(p_ss.data(), size);

  for (int32_t i = 0; i != size; ++i) {
    connection_hdl hdl = handles[i];
    auto result = ss[i]->GetResult();
    hdl->send(result.AsJsonString());
  }
}

void OfflineWebsocketServerConfig::Register(ParseOptions *po) {
  decoder_config.Register(po);
  po->Register("log-file", &log_file,
               "Path to the log file. Logs are appended to this file");
}

void OfflineWebsocketServerConfig::Validate() const {
  decoder_config.Validate();
}

OfflineWebsocketServer::OfflineWebsocketServer(
    hv::EventLoopThreadPool* io_work,  // NOLINT
    const OfflineWebsocketServerConfig &config)
    : io_work_(io_work),
      config_(config),
      log_(config.log_file, std::ios::app),
      tee_(std::cout, log_),
      decoder_(this) {
  onopen = [this](const WebSocketChannelPtr& hdl, const HttpRequestPtr&) {OnOpen(hdl); };
  onclose = [this](const WebSocketChannelPtr &hdl) { OnClose(hdl); };
  onmessage = [this](const WebSocketChannelPtr &hdl, const std::string& msg) {OnMessage(hdl, msg);};
}

void OfflineWebsocketServer::OnOpen(connection_hdl hdl) {
  hdl->newContextPtr<ConnectionData>();
}

void OfflineWebsocketServer::OnClose(connection_hdl hdl) {
  hdl->deleteContextPtr();
}

void OfflineWebsocketServer::OnMessage(connection_hdl hdl,
                                       const std::string &payload) {

  switch (hdl->opcode) {
    case WS_OPCODE_TEXT:
      if (payload == "Done") {
        // The client will not send any more data. We can close the
        // connection now.
        Close(hdl, "Done");
      } else {
        Close(hdl, std::string("Invalid payload: ") + payload);
      }
      break;

    case WS_OPCODE_BINARY: {
      auto connection_data = hdl->getContextPtr<ConnectionData>();
      auto p = reinterpret_cast<const int8_t *>(payload.data());

      if (connection_data->expected_byte_size == 0) {
        if (payload.size() < 8) {
          Close(hdl, "Payload is too short");
          break;
        }

        connection_data->sample_rate = *reinterpret_cast<const int32_t *>(p);
        connection_data->expected_byte_size = *reinterpret_cast<const int32_t *>(p + 4);

        int32_t max_byte_size_ = decoder_.GetConfig().max_utterance_length *
                                 connection_data->sample_rate * sizeof(float);
        if (connection_data->expected_byte_size > max_byte_size_) {
          float num_samples = connection_data->expected_byte_size / sizeof(float);
          float duration = num_samples / connection_data->sample_rate;

          std::ostringstream os;
          os << "Max utterance length is configured to "
             << decoder_.GetConfig().max_utterance_length
             << " seconds, received length is " << duration << " seconds. "
             << "Payload is too large!";
          Close(hdl, os.str());
          break;
        }

        connection_data->data.resize(connection_data->expected_byte_size);
        std::copy(payload.begin() + 8, payload.end(), connection_data->data.data());
        connection_data->cur = payload.size() - 8;
      } else {
        std::copy(payload.begin(), payload.end(), connection_data->data.data() + connection_data->cur);
        connection_data->cur += payload.size();
      }

      if (connection_data->expected_byte_size == connection_data->cur) {
        auto d = std::make_shared<ConnectionData>(std::move(*connection_data));
        // Clear it so that we can handle the next audio file from the client.
        // The client can send multiple audio files for recognition without
        // the need to create another connection.
        connection_data->sample_rate = 0;
        connection_data->expected_byte_size = 0;
        connection_data->cur = 0;

        decoder_.Push(hdl, d);

        connection_data->Clear();

        io_work_->loop()->runInLoop([this]() { decoder_.Decode(); });
      }
      break;
    }

    default:
      // Unexpected message, ignore it
      break;
  }
}

void OfflineWebsocketServer::Close(connection_hdl hdl,
                                   //websocketpp::close::status::value code,
                                   const std::string &reason) {
  std::cout << "Closing " << hdl->peeraddr() << " with reason: " << reason << "\n";
  hdl->close();
}

}  // namespace sherpa_onnx
