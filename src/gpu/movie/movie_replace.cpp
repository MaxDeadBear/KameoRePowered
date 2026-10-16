#include "gpu/movie/movie_replace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <xaudio2.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/log.h"

REXCVAR_DEFINE_BOOL(kameo_movie_replacements, true, "Kameo", "Play <kameo_movie_dir>/<movie name>.mp4 (or .m4v, .mov, .mkv) in place of the matching Bink movie. One with a soundtrack plays its own sound for its own length; one without only replaces the picture.");
REXCVAR_DEFINE_STRING(kameo_movie_dir, "movies", "Kameo", "Folder holding replacement movies, relative to the working directory.");

namespace kameo::gpu {
#ifndef _WIN32
MovieDrive DriveReplacementMovie(const std::string&, uint32_t, bool, float) {
  return MovieDrive::Bink;
}
bool ReplaceMovieFrame(const std::string&, uint32_t, double, BinkFrame*) {
  return false;
}
void EndReplacementMovie(uint32_t) {}
#else
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

constexpr int64_t kTicksPerSecond = 10000000;
constexpr size_t kFramesAhead = 4;
constexpr auto kFirstFrameWait = std::chrono::milliseconds(250);
constexpr auto kOpenWait = std::chrono::milliseconds(1000);
constexpr auto kAudioPrimeWait = std::chrono::milliseconds(2000);

struct MovieFrame {
  int64_t pts = 0;
  int64_t duration = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t chroma_width = 0;
  uint32_t chroma_height = 0;
  std::shared_ptr<std::vector<uint8_t>> planes;
  float yuv_to_rgb[16] = {};
};

struct VideoFormat {
  UINT32 width = 0;
  UINT32 height = 0;
  LONG stride = 0;
  uint32_t crop_x = 0;
  uint32_t crop_y = 0;
  uint32_t crop_width = 0;
  uint32_t crop_height = 0;
  int64_t frame_ticks = kTicksPerSecond / 30;
  float yuv_to_rgb[16] = {};
};

// Rows dotted with (y, cr, cb, 1), the shape of the guest's own yuvtorgb matrix the Bink shader applies.
void BuildYuvToRgb(bool bt709, bool full_range, float* m) {
  const float ys = full_range ? 1.0f : 255.0f / 219.0f;
  const float cs = full_range ? 1.0f : 255.0f / 224.0f;
  const float yo = full_range ? 0.0f : 16.0f / 255.0f;
  const float co = 128.0f / 255.0f;
  const float r_cr = bt709 ? 1.5748f : 1.402f;
  const float g_cb = bt709 ? 0.1873f : 0.344136f;
  const float g_cr = bt709 ? 0.4681f : 0.714136f;
  const float b_cb = bt709 ? 1.8556f : 1.772f;
  const float rows[4][4] = {
      {ys, r_cr * cs, 0.0f, -ys * yo - r_cr * cs * co},
      {ys, -g_cr * cs, -g_cb * cs, -ys * yo + (g_cr + g_cb) * cs * co},
      {ys, 0.0f, b_cb * cs, -ys * yo - b_cb * cs * co},
      {0.0f, 0.0f, 0.0f, 1.0f},
  };
  std::memcpy(m, rows, sizeof(rows));
}

bool ReadFormat(IMFSourceReader* reader, VideoFormat* format) {
  ComPtr<IMFMediaType> type;
  if (FAILED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type))) return false;
  if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &format->width, &format->height)) || !format->width || !format->height) return false;
  format->stride = LONG(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, format->width));
  format->crop_x = 0;
  format->crop_y = 0;
  format->crop_width = format->width;
  format->crop_height = format->height;
  // Decoders pad to whole macroblocks (1088 lines for 1080p); the display aperture is the picture inside.
  MFVideoArea area = {};
  if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
    const uint32_t x = uint32_t(std::max<int>(area.OffsetX.value, 0));
    const uint32_t y = uint32_t(std::max<int>(area.OffsetY.value, 0));
    if (area.Area.cx > 0 && area.Area.cy > 0 && x + uint32_t(area.Area.cx) <= format->width && y + uint32_t(area.Area.cy) <= format->height) {
      format->crop_x = x & ~1u;
      format->crop_y = y & ~1u;
      format->crop_width = uint32_t(area.Area.cx);
      format->crop_height = uint32_t(area.Area.cy);
    }
  }
  UINT32 numerator = 0;
  UINT32 denominator = 0;
  if (SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)) && numerator && denominator) format->frame_ticks = kTicksPerSecond * denominator / numerator;
  const UINT32 matrix = MFGetAttributeUINT32(type.Get(), MF_MT_YUV_MATRIX, MFVideoTransferMatrix_Unknown);
  const UINT32 range = MFGetAttributeUINT32(type.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Unknown);
  const bool bt709 = matrix == MFVideoTransferMatrix_BT709 || (matrix != MFVideoTransferMatrix_BT601 && format->crop_height > 576);
  BuildYuvToRgb(bt709, range == MFNominalRange_0_255, format->yuv_to_rgb);
  return true;
}

// NV12 into the Y, Cr and Cb planes the Bink upload takes, cropped to the display aperture.
bool ConvertSample(IMFSample* sample, const VideoFormat& format, MovieFrame* out) {
  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
  ComPtr<IMF2DBuffer> buffer2d;
  BYTE* scan0 = nullptr;
  LONG pitch = 0;
  DWORD length = 0;
  const bool locked2d = SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch));
  if (!locked2d) {
    if (FAILED(buffer->Lock(&scan0, nullptr, &length))) return false;
    pitch = std::abs(format.stride);
  }
  const auto unlock = [&] { locked2d ? buffer2d->Unlock2D() : buffer->Unlock(); };
  if (!scan0 || pitch < LONG(format.width) || (!locked2d && length < size_t(pitch) * format.height * 3 / 2)) {
    unlock();
    return false;
  }
  const uint32_t width = format.crop_width;
  const uint32_t height = format.crop_height;
  const uint32_t chroma_width = (width + 1) / 2;
  const uint32_t chroma_height = (height + 1) / 2;
  auto planes = std::make_shared<std::vector<uint8_t>>(size_t(width) * height + size_t(chroma_width) * chroma_height * 2);
  uint8_t* y = planes->data();
  uint8_t* cr = y + size_t(width) * height;
  uint8_t* cb = cr + size_t(chroma_width) * chroma_height;
  for (uint32_t row = 0; row < height; ++row) std::memcpy(y + size_t(row) * width, scan0 + size_t(format.crop_y + row) * pitch + format.crop_x, width);
  const BYTE* uv = scan0 + size_t(pitch) * format.height;
  for (uint32_t row = 0; row < chroma_height; ++row) {
    const BYTE* line = uv + size_t(format.crop_y / 2 + row) * pitch + format.crop_x;
    for (uint32_t column = 0; column < chroma_width; ++column) {
      cb[size_t(row) * chroma_width + column] = line[column * 2];
      cr[size_t(row) * chroma_width + column] = line[column * 2 + 1];
    }
  }
  unlock();
  out->width = width;
  out->height = height;
  out->chroma_width = chroma_width;
  out->chroma_height = chroma_height;
  out->planes = std::move(planes);
  std::memcpy(out->yuv_to_rgb, format.yuv_to_rgb, sizeof(out->yuv_to_rgb));
  return true;
}

class MediaFoundationThread {
 public:
  MediaFoundationThread() : com_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))), mf_(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {}
  ~MediaFoundationThread() {
    if (mf_) MFShutdown();
    if (com_) CoUninitialize();
  }
  bool ok() const { return mf_; }

 private:
  bool com_;
  bool mf_;
};

// One replacement movie: pictures decoded on one thread a few frames ahead of the time asked for, and, when the file has a soundtrack, its sound played on another as the clock those pictures follow.
class MoviePlayer {
 public:
  MoviePlayer(std::filesystem::path path, std::string name, uint32_t handle) : path_(std::move(path)), name_(std::move(name)), handle_(handle) {
    video_thread_ = std::thread([this] { RunVideo(); });
    audio_thread_ = std::thread([this] { RunAudio(); });
  }

  ~MoviePlayer() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    video_thread_.join();
    audio_thread_.join();
  }

  bool Matches(const std::string& name, uint32_t handle) const { return handle == handle_ && name == name_; }
  uint32_t handle() const { return handle_; }
  void SetGain(float gain) { gain_.store(gain, std::memory_order_relaxed); }

  // Waits for the file to be opened; false when it could not be.
  bool WaitReady(bool* has_audio) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, kOpenWait, [&] { return ready_; });
    *has_audio = has_audio_;
    return ready_ && !failed_;
  }

  bool HasAudio() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_ && has_audio_;
  }

  // Seconds into the movie by its soundtrack; once the sound has run out, the wall clock carries on from there.
  double ClockSeconds() {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    if (audio_done_) return audio_end_seconds_ + std::chrono::duration<double>(Clock::now() - audio_done_time_).count();
    if (!voice_ || !sample_rate_) return 0.0;
    XAUDIO2_VOICE_STATE state = {};
    voice_->GetState(&state, 0);
    return double(state.SamplesPlayed) / double(sample_rate_);
  }

  bool Finished() {
    const double clock = ClockSeconds();
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_) return true;
    if (duration_ > 0) return clock * double(kTicksPerSecond) >= double(duration_);
    std::lock_guard<std::mutex> audio_lock(audio_mutex_);
    return ended_ && audio_done_;
  }

  bool FrameAt(int64_t pts, MovieFrame* out) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (failed_) return false;
    const int64_t frame_ticks = frames_.empty() ? kTicksPerSecond / 30 : std::max<int64_t>(frames_.front().duration, 1);
    const int64_t oldest = frames_.empty() ? wanted_ : frames_.front().pts;
    const int64_t newest = frames_.empty() ? wanted_ : frames_.back().pts;
    bool seeked = false;
    // A jump back, or further ahead than decoding would catch up with.
    if ((delivered_ || !frames_.empty()) && (pts < oldest - 2 * frame_ticks || pts > newest + 2 * kTicksPerSecond)) {
      seek_ = true;
      seek_to_ = pts;
      frames_.clear();
      seeked = true;
    }
    wanted_ = pts;
    TrimLocked();
    cv_.notify_all();
    const auto pick = [&]() -> const MovieFrame* {
      const MovieFrame* best = nullptr;
      for (const MovieFrame& frame : frames_) {
        if (frame.pts > pts + frame.duration / 2) break;
        best = &frame;
      }
      if (!best && !frames_.empty() && frames_.front().pts - pts <= 2 * frame_ticks) best = &frames_.front();
      return best;
    };
    const MovieFrame* best = pick();
    if (!best && (!delivered_ || seeked)) {
      cv_.wait_for(lock, kFirstFrameWait, [&] { return failed_ || pick() != nullptr; });
      best = failed_ ? nullptr : pick();
    }
    if (!best) return false;
    *out = *best;
    delivered_ = true;
    return true;
  }

 private:
  size_t FramesAheadLocked() const {
    return size_t(std::count_if(frames_.begin(), frames_.end(), [&](const MovieFrame& frame) { return frame.pts > wanted_; }));
  }

  void TrimLocked() {
    while (frames_.size() >= 2 && frames_[1].pts <= wanted_) frames_.pop_front();
  }

  void Fail(const char* why) {
    KGFX_WARN("movie replacement {}: {}; the Bink plays instead", path_.string(), why);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      failed_ = true;
      ready_ = true;
    }
    cv_.notify_all();
  }

  // The soundtrack is over, or could not be played: the clock carries on by the wall clock from where the sound got to.
  void MarkAudioDone(double seconds) {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    if (audio_done_) return;
    audio_end_seconds_ = seconds;
    audio_done_time_ = Clock::now();
    audio_done_ = true;
  }

  void RunVideo() {
    MediaFoundationThread mf;
    if (!mf.ok()) return Fail("Media Foundation is unavailable");
    DecodeVideo();
  }

  void DecodeVideo() {
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateAttributes(&attributes, 1)) || FAILED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE)) || FAILED(MFCreateSourceReaderFromURL(path_.c_str(), attributes.Get(), &reader))) return Fail("could not be opened");
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    ComPtr<IMFMediaType> nv12;
    if (FAILED(MFCreateMediaType(&nv12)) || FAILED(nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) || FAILED(nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) || FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, nv12.Get()))) return Fail("has no video stream Windows can decode");
    VideoFormat format;
    if (!ReadFormat(reader.Get(), &format)) return Fail("reports no picture size");
    ComPtr<IMFMediaType> audio_type;
    const bool has_audio = SUCCEEDED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &audio_type));
    int64_t duration = 0;
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value)) && value.vt == VT_UI8) duration = int64_t(value.uhVal.QuadPart);
    PropVariantClear(&value);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      has_audio_ = has_audio;
      duration_ = duration;
      ready_ = true;
    }
    cv_.notify_all();
    KGFX_INFO("movie replacement {}: {}x{}, {:.1f}s, {}", path_.string(), format.crop_width, format.crop_height, double(duration) / kTicksPerSecond, has_audio ? "with its own sound" : "picture only, sound from the Bink");
    while (true) {
      int64_t seek_to = -1;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return stop_ || seek_ || (!ended_ && FramesAheadLocked() < kFramesAhead); });
        if (stop_) return;
        if (seek_) {
          seek_ = false;
          seek_to = seek_to_;
          ended_ = false;
          frames_.clear();
        }
      }
      if (seek_to >= 0) {
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = std::max<int64_t>(seek_to, 0);
        reader->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
      }
      DWORD flags = 0;
      LONGLONG timestamp = 0;
      ComPtr<IMFSample> sample;
      if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &timestamp, &sample))) return Fail("failed to decode");
      if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && !ReadFormat(reader.Get(), &format)) return Fail("changed to a format without a picture size");
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        std::lock_guard<std::mutex> lock(mutex_);
        ended_ = true;
        continue;
      }
      if (!sample) continue;
      MovieFrame frame;
      if (!ConvertSample(sample.Get(), format, &frame)) continue;
      frame.pts = timestamp;
      LONGLONG sample_duration = 0;
      frame.duration = SUCCEEDED(sample->GetSampleDuration(&sample_duration)) && sample_duration > 0 ? sample_duration : format.frame_ticks;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (seek_) continue;
        frames_.push_back(std::move(frame));
        TrimLocked();
      }
      cv_.notify_all();
    }
  }

  void RunAudio() {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return stop_ || ready_; });
      if (stop_ || failed_ || !has_audio_) return;
    }
    MediaFoundationThread mf;
    if (!mf.ok()) return MarkAudioDone(0.0);
    PlayAudio();
  }

  void PlayAudio() {
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> pcm;
    ComPtr<IMFMediaType> current;
    if (FAILED(MFCreateSourceReaderFromURL(path_.c_str(), nullptr, &reader)) || FAILED(MFCreateMediaType(&pcm)) || FAILED(pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) || FAILED(pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float))) return AudioFailed("could not open its soundtrack");
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
    if (FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pcm.Get())) || FAILED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &current))) return AudioFailed("has a soundtrack Windows cannot decode");
    WAVEFORMATEX* wave = nullptr;
    UINT32 wave_size = 0;
    if (FAILED(MFCreateWaveFormatExFromMFMediaType(current.Get(), &wave, &wave_size)) || !wave) return AudioFailed("reports no sound format");
    const WAVEFORMATEX format = *wave;
    ComPtr<IXAudio2> xaudio;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
    const bool created = SUCCEEDED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) && SUCCEEDED(xaudio->CreateMasteringVoice(&master)) && SUCCEEDED(xaudio->CreateSourceVoice(&voice, wave));
    CoTaskMemFree(wave);
    if (!created || !format.nSamplesPerSec || !format.nBlockAlign) {
      if (voice) voice->DestroyVoice();
      if (master) master->DestroyVoice();
      return AudioFailed("could not open an audio device");
    }
    {
      // The sound starts with the first picture, so the two leave together.
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, kAudioPrimeWait, [&] { return stop_ || failed_ || !frames_.empty() || delivered_; });
    }
    {
      std::lock_guard<std::mutex> lock(audio_mutex_);
      voice_ = voice;
      sample_rate_ = format.nSamplesPerSec;
    }
    voice->SetVolume(gain_.load(std::memory_order_relaxed));
    voice->Start();
    // Submitted buffers and the sample count each ends at; XAudio2 reads them in place until played.
    std::deque<std::pair<uint64_t, std::vector<uint8_t>>> queued;
    uint64_t submitted = 0;
    bool end_of_stream = false;
    bool first = true;
    while (true) {
      if (!first) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, std::chrono::milliseconds(10), [&] { return stop_; })) break;
      }
      first = false;
      voice->SetVolume(gain_.load(std::memory_order_relaxed));
      XAUDIO2_VOICE_STATE state = {};
      voice->GetState(&state, 0);
      while (!queued.empty() && queued.front().first <= state.SamplesPlayed) queued.pop_front();
      if (end_of_stream) {
        if (state.BuffersQueued == 0) MarkAudioDone(double(state.SamplesPlayed) / format.nSamplesPerSec);
        continue;
      }
      // Half a second queued ahead of what has played.
      while (!end_of_stream && submitted < state.SamplesPlayed + format.nSamplesPerSec / 2) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
          end_of_stream = true;
          break;
        }
        ComPtr<IMFMediaBuffer> buffer;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (!sample || FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&data, nullptr, &length))) continue;
        std::vector<uint8_t> bytes(data, data + length - length % format.nBlockAlign);
        buffer->Unlock();
        if (bytes.empty()) continue;
        XAUDIO2_BUFFER submit = {};
        submit.AudioBytes = UINT32(bytes.size());
        submit.pAudioData = bytes.data();
        submitted += bytes.size() / format.nBlockAlign;
        queued.emplace_back(submitted, std::move(bytes));
        if (FAILED(voice->SubmitSourceBuffer(&submit))) {
          queued.pop_back();
          end_of_stream = true;
        }
      }
    }
    voice->Stop();
    {
      std::lock_guard<std::mutex> lock(audio_mutex_);
      voice_ = nullptr;
    }
    voice->DestroyVoice();
    master->DestroyVoice();
  }

  void AudioFailed(const char* why) {
    KGFX_WARN("movie replacement {}: {}; it plays silent", path_.string(), why);
    MarkAudioDone(0.0);
  }

  const std::filesystem::path path_;
  const std::string name_;
  const uint32_t handle_;
  std::atomic<float> gain_{1.0f};
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<MovieFrame> frames_;
  int64_t wanted_ = 0;
  int64_t seek_to_ = 0;
  int64_t duration_ = 0;
  bool seek_ = false;
  bool stop_ = false;
  bool ended_ = false;
  bool failed_ = false;
  bool ready_ = false;
  bool has_audio_ = false;
  bool delivered_ = false;
  std::mutex audio_mutex_;
  IXAudio2SourceVoice* voice_ = nullptr;
  uint32_t sample_rate_ = 0;
  bool audio_done_ = false;
  double audio_end_seconds_ = 0.0;
  Clock::time_point audio_done_time_;
  std::thread video_thread_;
  std::thread audio_thread_;
};

std::filesystem::path FindReplacement(const std::string& name) {
  const std::filesystem::path dir(REXCVAR_GET(kameo_movie_dir));
  std::error_code ec;
  for (const char* extension : {".mp4", ".m4v", ".mov", ".mkv"}) {
    std::filesystem::path path = dir / (name + extension);
    if (std::filesystem::is_regular_file(path, ec)) return std::filesystem::absolute(path, ec);
  }
  return {};
}

std::mutex g_movieMutex;
std::unique_ptr<MoviePlayer> g_player;
// The movie last found to have no replacement, so the folder is not searched every frame.
std::string g_missingName;
uint32_t g_missingHandle = 0;

MoviePlayer* PlayerFor(const std::string& name, uint32_t handle) {
  if (!REXCVAR_GET(kameo_movie_replacements) || name.empty()) {
    g_player.reset();
    return nullptr;
  }
  if (g_player && g_player->Matches(name, handle)) return g_player.get();
  if (handle == g_missingHandle && name == g_missingName) return nullptr;
  g_player.reset();
  const std::filesystem::path path = FindReplacement(name);
  if (path.empty()) {
    g_missingName = name;
    g_missingHandle = handle;
    return nullptr;
  }
  g_missingName.clear();
  g_missingHandle = 0;
  KGFX_INFO("movie {}: replaced by {}", name, path.string());
  g_player = std::make_unique<MoviePlayer>(path, name, handle);
  return g_player.get();
}

}  // namespace

MovieDrive DriveReplacementMovie(const std::string& name, uint32_t bink_handle, bool has_alpha, float gain) {
  std::lock_guard<std::mutex> lock(g_movieMutex);
  MoviePlayer* player = has_alpha ? nullptr : PlayerFor(name, bink_handle);
  if (!player) return MovieDrive::Bink;
  player->SetGain(gain);
  bool has_audio = false;
  if (!player->WaitReady(&has_audio) || !has_audio) return MovieDrive::Bink;
  return player->Finished() ? MovieDrive::Finished : MovieDrive::Replaced;
}

bool ReplaceMovieFrame(const std::string& name, uint32_t bink_handle, double bink_seconds, BinkFrame* frame) {
  std::lock_guard<std::mutex> lock(g_movieMutex);
  MoviePlayer* player = frame->has_alpha ? nullptr : PlayerFor(name, bink_handle);
  if (!player) return false;
  const double seconds = player->HasAudio() ? player->ClockSeconds() : bink_seconds;
  MovieFrame movie;
  if (!player->FrameAt(int64_t(seconds * double(kTicksPerSecond)), &movie)) return false;
  frame->luma_width = movie.width;
  frame->luma_height = movie.height;
  frame->chroma_width = movie.chroma_width;
  frame->chroma_height = movie.chroma_height;
  const uint8_t* data = movie.planes->data();
  const size_t luma = size_t(movie.width) * movie.height;
  const size_t chroma = size_t(movie.chroma_width) * movie.chroma_height;
  frame->planes[0] = data;
  frame->planes[1] = data + luma;
  frame->planes[2] = data + luma + chroma;
  frame->planes[3] = nullptr;
  frame->pitches[0] = movie.width;
  frame->pitches[1] = movie.chroma_width;
  frame->pitches[2] = movie.chroma_width;
  frame->pitches[3] = 0;
  std::memcpy(frame->yuv_to_rgb, movie.yuv_to_rgb, sizeof(frame->yuv_to_rgb));
  frame->owned = std::move(movie.planes);
  return true;
}

void EndReplacementMovie(uint32_t bink_handle) {
  std::lock_guard<std::mutex> lock(g_movieMutex);
  if (g_player && g_player->handle() == bink_handle) g_player.reset();
  if (g_missingHandle == bink_handle) {
    g_missingHandle = 0;
    g_missingName.clear();
  }
}
#endif

}  // namespace kameo::gpu
