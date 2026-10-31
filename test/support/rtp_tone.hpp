// The tests' RTP sender and tone check (docs/design/rtp.md section 7.4), shared by the receiver's
// engine tests (src/pipewire/tests/rtp_receiver_test.cpp) and the system test
// (test/system/rtp_test.cpp). Header-only; the target ear_rtp_tone puts it on the include path.
// Interface stage: slice R1 writes the bodies.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

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

}  // namespace ear::test
