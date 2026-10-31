// The tests' RTP sender and tone check (docs/design/rtp.md section 7.4), shared by the receiver's
// engine tests (src/pipewire/tests/rtp_receiver_test.cpp) and the system test
// (test/system/rtp_test.cpp). Header-only; the target ear_rtp_tone puts it on the include path.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ear::test {

/// The payload the sender writes: big-endian signed PCM, 2 or 3 bytes a sample.
enum class RtpPcm { l16, l24 };

/// What the sender sends: channel i is a sine at frequencies[i] Hz with this peak amplitude
/// (0 < amplitude < 1), sample n of channel i = amplitude * sin(2 pi f_i n / rate), quantised.
struct RtpTone {
  std::vector<double> frequencies;   // one per channel, 1-64 channels
  double amplitude;
};

/// Sends one RTP stream (RFC 3550: version 2, payload type 96, the SSRC `ssrc`, sequence and
/// timestamp counting from fixed values) to 127.0.0.1:`port` from its own thread. A packet
/// carries 48 frames, or fewer when 48 would pass 1268 bytes of payload (PipeWire 1.4.2 reads a
/// datagram into a 1280-byte buffer). It sends at `rate` frames a second on the steady clock:
/// the stand-in for a sending device's own clock, which runs whatever EAR does (it paces the
/// stream; it never decides when a test step happens).
class RtpSender {
public:
  RtpSender(std::uint16_t port, RtpPcm pcm, std::uint32_t rate, RtpTone tone,
            std::uint32_t ssrc = 0xC0FFEE);
  ~RtpSender();   // stops sending and joins the thread
  RtpSender(const RtpSender&) = delete;
  RtpSender& operator=(const RtpSender&) = delete;

  /// Starts sending `frames` frames (a whole number of packets, rounded up) and returns at once.
  /// Once only.
  void send(std::uint64_t frames);
  /// The sender's thread has sent every frame; false when `deadline` passed first (fails a test,
  /// never paces one).
  bool waitSent(std::chrono::milliseconds deadline);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// A UDP port on 127.0.0.1 that no socket held a moment ago (bound to port 0, read back,
/// closed). Another process could take it in between; the tests accept that.
std::uint16_t freeUdpPort();

/// The least-squares fit of `samples` (recorded at `rate`) to one sine at `frequency`.
struct SineFit {
  double amplitude;   // fitted peak amplitude
  double residual;    // RMS of what the fitted sine leaves, divided by `amplitude`
};
SineFit fitSine(std::span<const float> samples, double frequency, std::uint32_t rate);

// ----- Bodies -----

namespace detail {

/// A UDP socket, closed when it goes.
class UdpSocket {
public:
  UdpSocket() : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {
    if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "socket");
  }
  ~UdpSocket() { ::close(fd_); }
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  int fd() const noexcept { return fd_; }

private:
  int fd_;
};

inline sockaddr_in loopback(std::uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return address;
}

}  // namespace detail

struct RtpSender::Impl {
  Impl(std::uint16_t port, RtpPcm pcm, std::uint32_t rate, RtpTone tone, std::uint32_t ssrc)
      : to(detail::loopback(port)), pcm(pcm), rate(rate), tone(std::move(tone)), ssrc(ssrc) {
    if (this->tone.frequencies.empty() || this->tone.frequencies.size() > 64)
      throw std::invalid_argument("RtpSender: 1-64 channels");
    const std::size_t frameBytes = this->tone.frequencies.size() * bytesPerSample();
    framesPerPacket = static_cast<std::uint32_t>(std::clamp<std::size_t>(maxPayload / frameBytes, 1, 48));
  }

  static constexpr std::size_t maxPayload = 1280 - 12;   // PipeWire's buffer less the RTP header

  std::size_t bytesPerSample() const { return pcm == RtpPcm::l24 ? 3 : 2; }

  /// The packet carrying frames [first, first + framesPerPacket).
  std::vector<std::uint8_t> packet(std::uint64_t first) const {
    std::vector<std::uint8_t> bytes(12);
    const std::uint16_t sequence = static_cast<std::uint16_t>(1234 + first / framesPerPacket);
    const std::uint32_t timestamp = static_cast<std::uint32_t>(99999 + first);
    bytes[0] = 0x80;   // version 2, no padding, extension or CSRC
    bytes[1] = 96;     // no marker, payload type 96 (dynamic)
    bytes[2] = static_cast<std::uint8_t>(sequence >> 8U);
    bytes[3] = static_cast<std::uint8_t>(sequence);
    for (int i = 0; i < 4; ++i) {
      bytes[4 + i] = static_cast<std::uint8_t>(timestamp >> (24 - 8 * i));
      bytes[8 + i] = static_cast<std::uint8_t>(ssrc >> (24 - 8 * i));
    }
    const double full = pcm == RtpPcm::l24 ? 8388607.0 : 32767.0;
    for (std::uint64_t frame = first; frame < first + framesPerPacket; ++frame) {
      for (const double frequency : tone.frequencies) {
        const double value = tone.amplitude * std::sin(2.0 * std::numbers::pi * frequency *
                                                       static_cast<double>(frame) / rate);
        const auto sample = static_cast<std::int32_t>(std::lrint(value * full));
        if (pcm == RtpPcm::l24) bytes.push_back(static_cast<std::uint8_t>(sample >> 16));
        bytes.push_back(static_cast<std::uint8_t>(sample >> 8));
        bytes.push_back(static_cast<std::uint8_t>(sample));
      }
    }
    return bytes;
  }

  /// The sending thread: each packet at the moment its last frame is due on the steady clock (the
  /// device clock's pace, never a test step), one wake a packet, until `total` frames are sent
  /// or the sender goes. Sending on an absolute schedule keeps the arrival jitter low: the
  /// receiver steers its resampler by the packets' arrival.
  void run(std::uint64_t total) {
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t sent = 0; sent < total && !stopping.load(); sent += framesPerPacket) {
      const std::chrono::duration<double> due(static_cast<double>(sent + framesPerPacket) / rate);
      std::this_thread::sleep_until(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(due));
      const auto bytes = packet(sent);
      ::sendto(socket.fd(), bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }
    const std::lock_guard lock(mutex);
    done = true;
    doneChanged.notify_all();
  }

  detail::UdpSocket socket;
  sockaddr_in to;
  RtpPcm pcm;
  std::uint32_t rate;
  RtpTone tone;
  std::uint32_t ssrc;
  std::uint32_t framesPerPacket = 48;
  std::atomic<bool> stopping{false};
  std::thread thread;
  std::mutex mutex;
  std::condition_variable doneChanged;
  bool done = false;
};

inline RtpSender::RtpSender(std::uint16_t port, RtpPcm pcm, std::uint32_t rate, RtpTone tone, std::uint32_t ssrc)
    : impl_(std::make_unique<Impl>(port, pcm, rate, std::move(tone), ssrc)) {}

inline RtpSender::~RtpSender() {
  impl_->stopping = true;
  if (impl_->thread.joinable()) impl_->thread.join();
}

inline void RtpSender::send(std::uint64_t frames) {
  if (impl_->thread.joinable()) throw std::logic_error("RtpSender::send: once only");
  const std::uint64_t packets = (frames + impl_->framesPerPacket - 1) / impl_->framesPerPacket;
  impl_->thread = std::thread([impl = impl_.get(), total = packets * impl_->framesPerPacket] { impl->run(total); });
}

inline bool RtpSender::waitSent(std::chrono::milliseconds deadline) {
  std::unique_lock lock(impl_->mutex);
  return impl_->doneChanged.wait_for(lock, deadline, [&] { return impl_->done; });
}

inline std::uint16_t freeUdpPort() {
  detail::UdpSocket socket;
  sockaddr_in address = detail::loopback(0);
  if (::bind(socket.fd(), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0)
    throw std::system_error(errno, std::generic_category(), "bind");
  socklen_t length = sizeof address;
  if (::getsockname(socket.fd(), reinterpret_cast<sockaddr*>(&address), &length) != 0)
    throw std::system_error(errno, std::generic_category(), "getsockname");
  return ntohs(address.sin_port);
}

inline SineFit fitSine(std::span<const float> samples, double frequency, std::uint32_t rate) {
  // samples[n] ~ a cos(w n) + b sin(w n): solve the 2x2 normal equations.
  const double w = 2.0 * std::numbers::pi * frequency / rate;
  double cc = 0.0, ss = 0.0, cs = 0.0, yc = 0.0, ys = 0.0;
  for (std::size_t n = 0; n < samples.size(); ++n) {
    const double c = std::cos(w * static_cast<double>(n));
    const double s = std::sin(w * static_cast<double>(n));
    cc += c * c;
    ss += s * s;
    cs += c * s;
    yc += samples[n] * c;
    ys += samples[n] * s;
  }
  const double determinant = cc * ss - cs * cs;
  if (samples.empty() || determinant == 0.0) return {0.0, INFINITY};
  const double a = (yc * ss - ys * cs) / determinant;
  const double b = (ys * cc - yc * cs) / determinant;
  double leftover = 0.0;
  for (std::size_t n = 0; n < samples.size(); ++n) {
    const double e = samples[n] - a * std::cos(w * static_cast<double>(n)) - b * std::sin(w * static_cast<double>(n));
    leftover += e * e;
  }
  const double amplitude = std::hypot(a, b);
  const double rms = std::sqrt(leftover / static_cast<double>(samples.size()));
  return {amplitude, amplitude > 0.0 ? rms / amplitude : INFINITY};
}

}  // namespace ear::test
