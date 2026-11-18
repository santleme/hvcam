// mf_capture — Media Foundation capture into FrameSlots.
//
// Key property vs. ad-hoc capture: the native media type is
// negotiated ONCE, before streaming (GetNativeMediaType +
// SetCurrentMediaType). There is no runtime renegotiation on this
// path — the failure mode where a driver hangs inside a format
// change is designed out, not patched around.
#pragma once

#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

class FrameSlots;

struct MfCameraInfo {
    int index;
    std::wstring name;
    std::wstring symlink;
};

struct MfOpenOptions {
    int device_index = 0;
    int width = 0;   // 0 = camera default
    int height = 0;
    int fps = 0;     // 0 = camera default
    bool lock_exposure = false;
    bool lock_focus = false;
    bool lock_gain = false;    // VideoProcAmp_Gain -> manual
    // CameraControl_Exposure units are log2 seconds: -5 ≈ 1/32 s,
    // -6 = 1/64 s. Below ~-5 the sensor can actually sustain 30 fps —
    // auto-exposure stretches to 1/20 s indoors (low-light comp) and
    // caps the stream near 20 fps. INT32_MIN = leave at auto.
    int exposure = INT32_MIN;
    // VideoProcAmp_Gain, device units; INT32_MIN = leave at auto.
    int gain = INT32_MIN;
    // Optional hard filter: restrict negotiation to this subtype.
    // If the camera offers no matching type, open() fails —
    // --format never silently degrades into another format.
    // 0 = auto (highest delivered frame rate at the requested size;
    // see include/media_score.h).
    uint32_t prefer_format = 0;
};

class MfCapture {
public:
    MfCapture();
    ~MfCapture();

    // Enumerates UVC devices. Works without opening the camera.
    static std::vector<MfCameraInfo> list_devices();

    // Opens the device and negotiates the media type. Throws
    // std::runtime_error on failure. After open(), format()/width()/
    // height() report what was actually negotiated.
    void open(const MfOpenOptions &opts);

    // Starts the async pump: each delivered frame is written into
    // `slots` (newest-wins) on an MF worker thread. Returns after the
    // first ReadSample is armed.
    void start(FrameSlots *slots);

    void close();

    int width() const;
    int height() const;
    uint32_t format() const; // enum HvPixelFormat
    int fps() const;

private:
    struct Impl;
    Impl *p_;
};
