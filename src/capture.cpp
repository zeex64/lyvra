#include "capture.h"

#include <linux/videodev2.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

uint64_t nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r == -1 && errno == EINTR);
    return r;
}

const char* fourccName(uint32_t f) {
    switch (f) {
        case V4L2_PIX_FMT_NV12: return "NV12";
        case V4L2_PIX_FMT_YUYV: return "YUYV";
        case V4L2_PIX_FMT_YUV420: return "I420";
        case V4L2_PIX_FMT_MJPEG: return "MJPEG";
        default: return "?";
    }
}

bool fourccSupported(uint32_t f) {
    return f == V4L2_PIX_FMT_NV12 || f == V4L2_PIX_FMT_YUYV || f == V4L2_PIX_FMT_YUV420;
}

double fourccBytesPerPixel(uint32_t f) { return f == V4L2_PIX_FMT_YUYV ? 2.0 : 1.5; }

static int fourccRank(uint32_t f) {  // lower = preferred (less USB bandwidth, simplest decode)
    switch (f) {
        case V4L2_PIX_FMT_NV12: return 0;
        case V4L2_PIX_FMT_YUV420: return 1;
        case V4L2_PIX_FMT_YUYV: return 2;
        default: return 9;
    }
}

std::string VideoMode::label() const {
    char b[96];
    double f = fps();
    if (std::fabs(f - std::round(f)) < 0.01) snprintf(b, sizeof b, "%u×%u @ %.0f · %s", width, height, f, fourccName(fourcc));
    else snprintf(b, sizeof b, "%u×%u @ %.2f · %s", width, height, f, fourccName(fourcc));
    return b;
}

std::string DeviceInfo::summary() const {
    if (modes.empty()) return "No supported uncompressed formats";
    std::set<uint32_t> fmts;
    for (auto& m : modes) fmts.insert(m.fourcc);
    char b[128];
    snprintf(b, sizeof b, "Up to %u×%u · %.0f fps max · ", maxWidth, maxHeight, maxFps);
    std::string s = b;
    bool first = true;
    for (int r = 0; r < 10; r++)
        for (auto f : fmts)
            if (fourccRank(f) == r) { s += (first ? "" : " / "); s += fourccName(f); first = false; }
    return s;
}

VideoMode pickBestMode(const std::vector<VideoMode>& modes) {
    // Prefer >=1080p if offered; then highest frame rate; then resolution; then cheapest format.
    bool has1080 = std::any_of(modes.begin(), modes.end(), [](auto& m) { return m.height >= 1080; });
    const VideoMode* best = nullptr;
    auto better = [](const VideoMode& a, const VideoMode& b) {
        if (std::fabs(a.fps() - b.fps()) > 0.5) return a.fps() > b.fps();
        if (a.width * a.height != b.width * b.height) return a.width * a.height > b.width * b.height;
        return fourccRank(a.fourcc) < fourccRank(b.fourcc);
    };
    for (auto& m : modes) {
        if (has1080 && m.height < 1080) continue;
        if (has1080 && m.height > 1080 && m.fps() < 59) continue;  // don't trade 60+ fps for resolution
        if (!best || better(m, *best)) best = &m;
    }
    if (!best && !modes.empty()) best = &modes[0];
    return best ? *best : VideoMode{};
}

std::string usbSpeedFor(const std::string& devPath) {
    // /sys/class/video4linux/videoN/device -> USB interface; parent has "speed" (Mbps)
    std::string node = fs::path(devPath).filename();
    std::error_code ec;
    fs::path p = fs::canonical("/sys/class/video4linux/" + node + "/device", ec);
    if (ec) return {};
    for (int i = 0; i < 3 && !p.empty(); i++, p = p.parent_path()) {
        std::ifstream f(p / "speed");
        int mbps = 0;
        if (f >> mbps && mbps > 0) {
            char b[32];
            if (mbps >= 1000) snprintf(b, sizeof b, "%g Gbps", mbps / 1000.0);
            else snprintf(b, sizeof b, "%d Mbps", mbps);
            return b;
        }
    }
    return {};
}

std::vector<DeviceInfo> enumerateDevices() {
    std::vector<DeviceInfo> out;
    std::vector<std::string> paths;
    std::error_code ec;
    for (auto& e : fs::directory_iterator("/dev", ec)) {
        auto n = e.path().filename().string();
        if (n.rfind("video", 0) == 0) paths.push_back(e.path());
    }
    std::sort(paths.begin(), paths.end(), [](auto& a, auto& b) {
        return std::atoi(a.c_str() + 10) < std::atoi(b.c_str() + 10);
    });

    for (auto& path : paths) {
        int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        v4l2_capability cap{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { ::close(fd); continue; }
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) { ::close(fd); continue; }

        DeviceInfo d;
        d.path = path;
        d.name = reinterpret_cast<const char*>(cap.card);
        d.bus = reinterpret_cast<const char*>(cap.bus_info);
        // "DX20R Video Device: DX20R Video" -> "DX20R Video Device"
        if (auto c = d.name.find(':'); c != std::string::npos) d.name = d.name.substr(0, c);

        v4l2_queryctrl qc{};
        qc.id = V4L2_CID_EXPOSURE_AUTO;
        bool hasExposure = xioctl(fd, VIDIOC_QUERYCTRL, &qc) == 0;
        qc = {};
        qc.id = V4L2_CID_EXPOSURE_ABSOLUTE;
        hasExposure = hasExposure || xioctl(fd, VIDIOC_QUERYCTRL, &qc) == 0;
        d.isCaptureCard = !hasExposure;

        v4l2_fmtdesc fd_{};
        fd_.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for (fd_.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &fd_) == 0; fd_.index++) {
            if (!fourccSupported(fd_.pixelformat)) continue;
            v4l2_frmsizeenum fs_{};
            fs_.pixel_format = fd_.pixelformat;
            for (fs_.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs_) == 0; fs_.index++) {
                if (fs_.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;
                v4l2_frmivalenum fi{};
                fi.pixel_format = fd_.pixelformat;
                fi.width = fs_.discrete.width;
                fi.height = fs_.discrete.height;
                for (fi.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0; fi.index++) {
                    if (fi.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
                    VideoMode m{fd_.pixelformat, fi.width, fi.height, fi.discrete.denominator, fi.discrete.numerator};
                    d.modes.push_back(m);
                    d.maxWidth = std::max(d.maxWidth, m.width);
                    if (m.width == d.maxWidth) d.maxHeight = std::max(d.maxHeight, m.height);
                    d.maxFps = std::max(d.maxFps, m.fps());
                }
            }
        }
        ::close(fd);
        if (d.modes.empty()) continue;  // metadata nodes, MJPEG-only devices
        std::stable_sort(d.modes.begin(), d.modes.end(), [](auto& a, auto& b) {
            if (a.width * a.height != b.width * b.height) return a.width * a.height > b.width * b.height;
            if (std::fabs(a.fps() - b.fps()) > 0.01) return a.fps() > b.fps();
            return fourccRank(a.fourcc) < fourccRank(b.fourcc);
        });
        out.push_back(std::move(d));
    }
    std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.isCaptureCard > b.isCaptureCard; });
    return out;
}

bool Capture::open(const std::string& devPath, const VideoMode& mode, std::string& err) {
    close();
    failed = false;
    failReason.clear();
    fd_ = ::open(devPath.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) { err = "Can't open " + devPath + ": " + strerror(errno); return false; }

    v4l2_capability cap{};
    xioctl(fd_, VIDIOC_QUERYCAP, &cap);
    name = reinterpret_cast<const char*>(cap.card);
    if (auto c = name.find(':'); c != std::string::npos) name = name.substr(0, c);
    path = devPath;

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = mode.width;
    fmt.fmt.pix.height = mode.height;
    fmt.fmt.pix.pixelformat = mode.fourcc;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) { err = std::string("VIDIOC_S_FMT failed: ") + strerror(errno); close(); return false; }
    if (!fourccSupported(fmt.fmt.pix.pixelformat)) { err = "Device picked an unsupported pixel format"; close(); return false; }
    width = fmt.fmt.pix.width;
    height = fmt.fmt.pix.height;
    fourcc = fmt.fmt.pix.pixelformat;
    stride = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline : width * (fourcc == V4L2_PIX_FMT_YUYV ? 2 : 1);
    sizeImage = fmt.fmt.pix.sizeimage;

    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = mode.fpsDen;
    parm.parm.capture.timeperframe.denominator = mode.fpsNum;
    xioctl(fd_, VIDIOC_S_PARM, &parm);
    xioctl(fd_, VIDIOC_G_PARM, &parm);
    auto& tpf = parm.parm.capture.timeperframe;
    fps = tpf.numerator ? double(tpf.denominator) / tpf.numerator : mode.fps();

    // 4 buffers: one in the app, one pending, two in flight in the driver.
    v4l2_requestbuffers req{};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) { err = "VIDIOC_REQBUFS failed"; close(); return false; }
    bufs_.resize(req.count);
    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) { err = "VIDIOC_QUERYBUF failed"; close(); return false; }
        bufs_[i].len = b.length;
        bufs_[i].ptr = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.m.offset);
        if (bufs_[i].ptr == MAP_FAILED) { bufs_[i].ptr = nullptr; err = "mmap failed"; close(); return false; }
        if (sizeImage == 0) sizeImage = b.length;
    }
    for (uint32_t i = 0; i < req.count; i++) queue(int(i));
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) { err = std::string("VIDIOC_STREAMON failed: ") + strerror(errno); close(); return false; }

    framesCaptured = framesSkipped = framesLostByDriver = 0;
    measuredFps = 0;
    lastFrameNs = 0;
    pending_ = {};
    stop_ = false;
    thread_ = std::thread(&Capture::run, this);
    return true;
}

void Capture::close() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
        for (auto& b : bufs_) if (b.ptr) munmap(b.ptr, b.len);
        bufs_.clear();
        v4l2_requestbuffers req{};
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        xioctl(fd_, VIDIOC_REQBUFS, &req);
        ::close(fd_);
        fd_ = -1;
    }
    pending_ = {};
}

void Capture::queue(int index) {
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = uint32_t(index);
    xioctl(fd_, VIDIOC_QBUF, &b);
}

bool Capture::acquireLatest(CaptureFrame& out) {
    std::lock_guard lk(mtx_);
    if (pending_.index < 0) return false;
    out = pending_;
    pending_ = {};
    return true;
}

void Capture::release(const CaptureFrame& f) {
    if (f.index >= 0 && fd_ >= 0) queue(f.index);
}

void Capture::run() {
    uint64_t times[128] = {};
    int head = 0, count = 0;
    uint32_t lastSeq = 0;
    bool haveSeq = false;

    while (!stop_) {
        pollfd p{fd_, POLLIN, 0};
        int r = poll(&p, 1, 100);
        if (r <= 0) continue;
        if (p.revents & (POLLERR | POLLHUP)) {
            failReason = "Device disconnected";
            failed = true;
            if (wake_) wake_();
            break;
        }
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) continue;
            failReason = std::string("Capture stopped: ") + strerror(errno);
            failed = true;
            if (wake_) wake_();
            break;
        }
        uint64_t deq = nowNs();
        if (b.flags & V4L2_BUF_FLAG_ERROR) { queue(int(b.index)); continue; }

        if (haveSeq && b.sequence > lastSeq + 1) framesLostByDriver += b.sequence - lastSeq - 1;
        lastSeq = b.sequence;
        haveSeq = true;

        CaptureFrame f;
        f.index = int(b.index);
        f.data = static_cast<const uint8_t*>(bufs_[b.index].ptr);
        f.bytes = b.bytesused ? b.bytesused : bufs_[b.index].len;
        f.timestampNs = uint64_t(b.timestamp.tv_sec) * 1000000000ull + uint64_t(b.timestamp.tv_usec) * 1000ull;
        if (f.timestampNs == 0 || f.timestampNs > deq) f.timestampNs = deq;
        f.dequeueNs = deq;
        f.sequence = b.sequence;

        {
            std::lock_guard lk(mtx_);
            if (pending_.index >= 0) { queue(pending_.index); framesSkipped++; }
            pending_ = f;
        }
        framesCaptured++;
        lastFrameNs = deq;

        // Frame rate and interval jitter over the last ~128 frames
        times[head] = f.timestampNs;
        head = (head + 1) % 128;
        if (count < 128) count++;
        if (count >= 8) {
            int n = count;
            uint64_t newest = times[(head + 127) % 128], oldest = times[(head + 128 - n) % 128];
            double span = double(newest - oldest) / 1e9;
            if (span > 0) measuredFps = (n - 1) / span;
            double mean = span / (n - 1), var = 0;
            for (int i = 1; i < n; i++) {
                double d = double(times[(head + 128 - n + i) % 128] - times[(head + 128 - n + i - 1) % 128]) / 1e9 - mean;
                var += d * d;
            }
            intervalJitterMs = std::sqrt(var / (n - 1)) * 1000.0;
        }
        if (wake_) wake_();
    }
}
