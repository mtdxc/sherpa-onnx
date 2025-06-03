// sherpa/cpp_api/websocket/online-websocket-client.cc
//
// Copyright (c)  2022  Xiaomi Corporation
#include <chrono>  // NOLINT
#include <fstream>
#include <string>

#include "sherpa-onnx/csrc/macros.h"
#include "sherpa-onnx/csrc/parse-options.h"
#include "sherpa-onnx/csrc/wave-reader.h"
#include "hv/WebSocketClient.h"

static constexpr const char *kUsageMessage = R"(
Automatic speech recognition with sherpa-onnx using websocket.

Usage:

./bin/sherpa-onnx-online-websocket-client --help

./bin/sherpa-onnx-online-websocket-client \
  --server-url=ws://127.0.0.1:6006 \
  --samples-per-message=8000 \
  --seconds-per-message=0.2 \
  /path/to/foo.wav

It support only wave of with a single channel, 16kHz, 16-bit samples.
)";

class Client : public hv::WebSocketClient {
 public:
  Client(const char* samples, int32_t size,
         int32_t bytes_per_message, float seconds_per_message)
      : samples_(samples, samples+size),
        bytes_per_message_(bytes_per_message),
        seconds_per_message_(seconds_per_message) {

    onopen = [this]() { 
        tid_ = hv::setInterval(seconds_per_message_ * 1000,
                             [this](hv::TimerID) { SendMessage(); });
    };
    onmessage = [this](const std::string &msg) {
      if (msg == "Done!") {
        close();
      } else {
        SHERPA_ONNX_LOGE("%s", msg.c_str());
      }
    };
  }

 private:

  void SendMessage() {
    if (send_pos_ < 1) {
      SHERPA_ONNX_LOGE("Starting to send audio");
    }
    int ret = samples_.size() - send_pos_;
    if (ret > bytes_per_message_) {
      ret = send(samples_.data() + send_pos_, bytes_per_message_);
      if (ret == -1) {
        SHERPA_ONNX_LOGE("Failed to send audio samples because %d", ret);
        exit(EXIT_FAILURE);
      }

      send_pos_+= bytes_per_message_;
    } else {
      if (tid_) {
        hv::killTimer(tid_);
        tid_ = 0;
      }

      if (ret) {
        ret = send(samples_.data() + send_pos_, ret);
        if (ret == -1) {
          SHERPA_ONNX_LOGE("Failed to send audio samples because %d", ret);
          exit(EXIT_FAILURE);
        }
        send_pos_ += ret;
      }

      // To signal that we have send all the messages
      ret = send("Done");
      SHERPA_ONNX_LOGE("Sent Done Signal");
      if (ret == -1) {
        SHERPA_ONNX_LOGE("Failed to send done because %d", ret);
        exit(EXIT_FAILURE);
      }
    }
  }

 private:
  hv::TimerID tid_;
  std::vector<char> samples_;
  int32_t bytes_per_message_ = 8000 * 4;  // 0.5 seconds
  float seconds_per_message_ = 0.2;
  int32_t send_pos_ = 0;
};

int32_t main(int32_t argc, char *argv[]) {
  std::string server_url = "ws://127.0.0.1:6006";

  // Sample rate of the input wave. No resampling is made.
  int32_t sample_rate = 16000;
  int32_t samples_per_message = 8000;
  float seconds_per_message = 0.2;

  sherpa_onnx::ParseOptions po(kUsageMessage);

  po.Register("server-url", &server_url, "IP address of the websocket server");
  po.Register("sample-rate", &sample_rate,
              "Sample rate of the input wave. Should be the one expected by "
              "the server");

  po.Register("samples-per-message", &samples_per_message,
              "Send this number of samples per message.");

  bool send_pcm = true;
  po.Register("send_short", &send_pcm, "Send with short or float.");
  po.Register("seconds-per-message", &seconds_per_message,
              "We will simulate that each message takes this number of seconds "
              "to send. If you select a very large value, it will take a long "
              "time to send all the samples");

  po.Read(argc, argv);

  if (server_url.empty()) {
    SHERPA_ONNX_LOGE("Invalid server url: %s", server_url.c_str());
    return -1;
  }

  // 0.01 is an arbitrary value. You can change it.
  if (samples_per_message <= 0.01 * sample_rate) {
    SHERPA_ONNX_LOGE("--samples-per-message is too small: %d",
                     samples_per_message);
    return -1;
  }

  // 100 is an arbitrary value. You can change it.
  if (samples_per_message >= sample_rate * 100) {
    SHERPA_ONNX_LOGE("--samples-per-message is too small: %d",
                     samples_per_message);
    return -1;
  }

  if (seconds_per_message < 0) {
    SHERPA_ONNX_LOGE("--seconds-per-message is too small: %.3f",
                     seconds_per_message);
    return -1;
  }

  // 1 is an arbitrary value.
  if (seconds_per_message > 1) {
    SHERPA_ONNX_LOGE(
        "--seconds-per-message is too large: %.3f. You will wait a long time "
        "to send all the samples",
        seconds_per_message);
    return -1;
  }

  if (po.NumArgs() != 1) {
    po.PrintUsage();
    return -1;
  }

  std::string wave_filename = po.GetArg(1);

  bool is_ok = false;
  int32_t actual_sample_rate = -1;
  std::vector<float> samples =
      sherpa_onnx::ReadWave(wave_filename, &actual_sample_rate, &is_ok);

  if (!is_ok) {
    SHERPA_ONNX_LOGE("Failed to read '%s'", wave_filename.c_str());
    return -1;
  }

  if (actual_sample_rate != sample_rate) {
    SHERPA_ONNX_LOGE("Expected sample rate: %d, given %d", sample_rate,
                     actual_sample_rate);
    return -1;
  }
  const char *data = (const char*)samples.data();
  int unit = sizeof(float);
  std::vector<short> pcms;
  if (send_pcm) {
    pcms.resize(samples.size());
    for (int i = 0; i < samples.size(); i++) {
      pcms[i] = samples[i] * 32768;
    }
    data = (const char *)pcms.data();
    unit = sizeof(short);
  }

  Client c(data, samples.size() * unit,  samples_per_message * unit, seconds_per_message);
  c.open(server_url.c_str());

  printf("presee q to exit\n");
  while (getchar() != 'q');

  SHERPA_ONNX_LOGE("Done!");
  return 0;
}
