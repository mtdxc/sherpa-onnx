// sherpa-onnx/csrc/file-utils.cc
//
// Copyright (c)  2022-2023  Xiaomi Corporation

#include "sherpa-onnx/csrc/file-utils.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "sherpa-onnx/csrc/macros.h"

namespace sherpa_onnx {

bool FileExists(const std::string &filename) {
  return std::ifstream(filename).good();
}

void AssertFileExists(const std::string &filename) {
  if (!FileExists(filename)) {
    SHERPA_ONNX_LOGE("filename '%s' does not exist", filename.c_str());
    exit(-1);
  }
}

std::vector<char> ReadFile(const std::string &filename) {
  std::ifstream input(filename, std::ios::binary);
  std::vector<char> buffer(std::istreambuf_iterator<char>(input), {});
  return buffer;
}

#if __ANDROID_API__ >= 9
std::vector<char> ReadFile(AAssetManager *mgr, const std::string &filename) {
  AAsset *asset = AAssetManager_open(mgr, filename.c_str(), AASSET_MODE_BUFFER);
  if (!asset) {
    __android_log_print(ANDROID_LOG_FATAL, "sherpa-onnx",
                        "Read binary file: Load '%s' failed", filename.c_str());
    return {};
  }

  auto p = reinterpret_cast<const char *>(AAsset_getBuffer(asset));
  size_t asset_length = AAsset_getLength(asset);

  std::vector<char> buffer(p, p + asset_length);
  AAsset_close(asset);

  return buffer;
}
#endif

#if __OHOS__
std::vector<char> ReadFile(NativeResourceManager *mgr,
                           const std::string &filename) {
  std::unique_ptr<RawFile, decltype(&OH_ResourceManager_CloseRawFile)> fp(
      OH_ResourceManager_OpenRawFile(mgr, filename.c_str()),
      OH_ResourceManager_CloseRawFile);

  if (!fp) {
    std::ostringstream os;
    os << "Read file '" << filename << "' failed.";
    SHERPA_ONNX_LOGE("%s", os.str().c_str());
    return {};
  }

  auto len = static_cast<int32_t>(OH_ResourceManager_GetRawFileSize(fp.get()));

  std::vector<char> buffer(len);

  int32_t n = OH_ResourceManager_ReadRawFile(fp.get(), buffer.data(), len);

  if (n != len) {
    std::ostringstream os;
    os << "Read file '" << filename << "' failed. Number of bytes read: " << n
       << ". Expected bytes to read: " << len;
    SHERPA_ONNX_LOGE("%s", os.str().c_str());
    return {};
  }

  return buffer;
}
#endif

AssetMgr &AssetMgr::Instance() {
  static AssetMgr sMgr;
  return sMgr;
}

void AssetMgr::Setup(const char *dir) {
  if (dir && dir[0])
    base_dir_ = dir;
  else
    base_dir_.clear();
}

std::vector<char> AssetMgr::ReadFile(const std::string &filename) {
  if (FileExists(filename)) {
    return ReadFile(filename);
  } 
  if (base_dir_.length()) {
    std::string path = base_dir_ + filename;
    if (FileExists(path)) return ReadFile(path);
  }
  std::vector<char> ret;
  if (mgr_) {
#if __ANDROID_API__ >= 9
    AAsset *asset = AAssetManager_open((AAssetManager *)mgr_, filename.c_str(),
                                       AASSET_MODE_BUFFER);
    if (asset) {
      auto p = reinterpret_cast<const char *>(AAsset_getBuffer(asset));
      size_t asset_length = AAsset_getLength(asset);
      ret.assign(p, p + asset_length);
      AAsset_close(asset);
    }
#endif
#if __OHOS__
    RawFile *fp = OH_ResourceManager_OpenRawFile((NativeResourceManager *)mgr_,
                                                 filename.c_str());
    if (fp) {
      auto len = static_cast<int32_t>(OH_ResourceManager_GetRawFileSize(fp));
      ret.resize(len);
      int32_t n = OH_ResourceManager_ReadRawFile(fp, ret.data(), len);
      if (n != len) {
        std::ostringstream os;
        os << "Read file '" << filename
           << "' failed. Number of bytes read: " << n
           << ". Expected bytes to read: " << len;
        SHERPA_ONNX_LOGE("%s", os.str().c_str());
        //ret.clear();
      }
      OH_ResourceManager_CloseRawFile(fp);
    }
#endif
  }
  return ret;
}

#if __ANDROID_API__ >= 9
void AssetMgr::Setup(jobject obj, JNIEnv *env) {
  jmgr_ = nullptr;
  mgr_ = nullptr;
  if (obj) {
    jmgr_ = std::shared_ptr<_jobject>(env->NewGlobalRef(obj),
                [env](jobject obj) { env->DeleteGlobalRef(obj); });
    mgr_ = AAssetManager_fromJava(env, jmgr_.get());
  }
}
#endif

}  // namespace sherpa_onnx
