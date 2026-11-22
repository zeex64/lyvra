#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

uint64_t nowNs();  // CLOCK_MONOTONIC, same clock as V4L2 buffer timestamps

struct VideoMode {
    uint32_t fourcc = 0, width = 0, height = 0;
    uint32_t fpsNum = 0, fpsDen = 1;  // frames per second = fpsNum / fpsDen
    double fps() const { return fpsDen ? double(fpsNum) / fpsDen : 0.0; }
    std::string label() const;
};

struct DeviceInfo {
    std::string path, name, bus;
    bool isCaptureCard = false;  // heuristic: no exposure control => not a camera
    std::vector<VideoMode> modes;  // only formats we can render, best first
    uint32_t maxWidth = 0, maxHeight = 0;
    double maxFps = 0;
    std::string summary() const;  // "Up to 1920×1080 @ 120 · NV12 / YUYV"
};

std::vector<DeviceInfo> enumerateDevices();
const char* fourccName(uint32_t fourcc);
bool fourccSupported(uint32_t fourcc);
double fourccBytesPerPixel(uint32_t fourcc);
VideoMode pickBestMode(const std::vector<VideoMode>& modes);
std::string usbSpeedFor(const std::string& devPath);  // e.g. "5 Gbps", empty if unknown

// Watches a directory (normally /dev) for video nodes appearing, disappearing or changing
// permissions, and calls `cb` from a background thread each time. The callback should only
// signal the main loop; enumerate there, after a short delay, since udev sets the node's group
// a moment after creating it.
class DeviceMonitor {
public:
    ~DeviceMonitor() { stop(); }
    bool start(std::function<void()> cb, const std::string& dir = "/dev");
    void stop();
    bool active() const { return fd_ >= 0; }

private:
    void run();
    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::function<void()> cb_;
};

struct CaptureFrame {
    int index = -1;  // V4L2 buffer index, or decode slot for MJPEG
    const uint8_t* data = nullptr;
    size_t bytes = 0;
    uint32_t fourcc = 0, stride = 0;  // layout of `data` (MJPEG arrives decoded to planar YUV)
    uint64_t timestampNs = 0;  // driver timestamp (frame start/end, monotonic)
    uint64_t dequeueNs = 0;    // when the app got it from the driver
    uint64_t decodeNs = 0;     // MJPEG decode time (0 for uncompressed formats)
    uint32_t sequence = 0;
};

class Capture {
public:
    ~Capture() { close(); }
    bool open(const std::string& path, const VideoMode& mode, std::string& err);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    bool compressed() const { return mjpeg_; }
    // USB payload rate: computed for uncompressed formats, measured for MJPEG (varies with content).
    double usbMBps() const {
        double bpp = fourccBytesPerPixel(fourcc);
        return (bpp > 0 ? width * height * bpp : bytesPerFrame.load()) * fps / 1e6;
    }

    // Takes the newest frame (older undelivered ones are recycled). Must be released.
    bool acquireLatest(CaptureFrame& out);
    void release(const CaptureFrame& f);

    void setWakeCallback(std::function<void()> cb) { wake_ = std::move(cb); }

    // Negotiated format
    uint32_t width = 0, height = 0, fourcc = 0, stride = 0, sizeImage = 0;
    double fps = 0;
    std::string path, name;

    // Stats (written by capture thread)
    std::atomic<uint64_t> framesCaptured{0}, framesSkipped{0}, framesLostByDriver{0};
    std::atomic<double> measuredFps{0};
    std::atomic<double> intervalJitterMs{0};
    std::atomic<double> bytesPerFrame{0};  // what actually crosses USB (smoothed)
    std::atomic<double> decodeMs{0};       // MJPEG decode time (smoothed)
    std::atomic<uint64_t> framesCorrupt{0};  // MJPEG frames that failed to decode
    std::atomic<uint64_t> lastFrameNs{0};
    std::atomic<bool> failed{false};
    std::string failReason;

private:
    void run();
    void queue(int index);
    bool decodeJpeg(const uint8_t* jpeg, size_t len, CaptureFrame& f);

    int fd_ = -1;
    struct Buf { void* ptr = nullptr; size_t len = 0; };
    std::vector<Buf> bufs_;
    // MJPEG: frames are decoded into these slots so the V4L2 buffer can go straight back to the driver.
    // 3 slots: one held by the app, one pending, one being decoded into.
    enum class Slot { Free, Decoding, Pending, InApp };
    struct DecodeSlot { std::vector<uint8_t> data; Slot state = Slot::Free; };
    DecodeSlot slots_[3];
    void* tj_ = nullptr;  // tjhandle
    bool mjpeg_ = false;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mtx_;
    CaptureFrame pending_;
    std::function<void()> wake_;
};
