# UsbCastReceiver

UsbCastReceiver is a Windows native proof-of-concept for a USB cast receiver device. It avoids Electron, Chromium, WebView, Qt, FFmpeg, ImGui, and third-party dependencies. The goal is to keep the render and audio paths as close as possible to Windows system multimedia components.

## Architecture

Video:

```text
UVC H.264 -> Media Foundation Source Reader -> DXVA decoder -> NV12 D3D11 texture -> D3D11 VideoProcessor -> DXGI swap chain
```

Audio:

```text
UAC PCM -> WASAPI capture -> WASAPI render
```

AirPlay migration scaffold:

```text
AirPlay DNS-SD -> RAOP/AirPlay protocol adapter -> MediaCore -> MF/D3D11/WASAPI renderers
```

HID AXTP media scaffold:

```text
HID report bytes -> AXTP Standard Frame reassembly -> STREAM parser -> MediaCore
```

The default video path uses Media Foundation Source Reader with an `IMFDXGIDeviceManager`, asks Windows to decode the UVC H.264 stream to NV12 DXVA-backed D3D11 textures, converts/scales through the D3D11 VideoProcessor, and presents through a DXGI swap chain bound to the child `HWND`. This is the working path for the tested `Wireless transceiver NA20` device. The Capture Engine preview path is still available with `--video-backend capture`, but it is treated as experimental because some UVC driver stacks fail or stay blank even when Source Reader can pull frames.

The audio path captures PCM from the selected UAC endpoint and writes it to the default render endpoint. Muting does not stop or reopen the capture device; the relay keeps draining capture buffers and writes silence to render.

The AirPlay migration is being landed incrementally. The current build has the shared media-core primitives and Bonjour/DNS-SD service registration for `_airplay._tcp` and `_raop._tcp`. The RAOP/HTTP/FairPlay server, AirPlay H.264 mirror decoder, and AirPlay audio path are not wired to playback yet.

The HID media path follows the AXTP Standard Framed profile used by the sibling `axtp` documents. HID transport, hidapi integration, Standard Frame parsing, L1 frame reassembly, and STREAM header parsing come from the `Mostorm-Labs/axtp-cpp-runtime` submodule. This app keeps only the product-side media adapter that maps `StreamPayload.cursor` values as `timestampUs` into the shared `MediaCore` clock and parses the draft audio/video chunk envelopes.

## Build

Use Windows 10/11 x64 with Visual Studio 2022. The AXTP C++ runtime is a required submodule dependency and supplies the HID transport, hidapi dependency, and AXTP protocol parser:

```bat
git submodule update --init --recursive
```

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

After pulling changes, rebuild the existing build tree or delete it and configure again. The app logs `UsbCastReceiver build git='...'` at startup; use that line to confirm the running executable contains the expected commit.

## Run

Run with default device selection:

```bat
build\Release\UsbCastReceiver.exe
```

Run with substring matching:

```bat
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --uac-match "audio name"
```

`--uvc-match` is matched case-insensitively against the UVC friendly name and symbolic link. `--uac-match` is matched case-insensitively against the UAC friendly name and device id. If a match argument is omitted or empty, the first active endpoint of that type is selected.

Source selection:

```bat
build\Release\UsbCastReceiver.exe --source auto
build\Release\UsbCastReceiver.exe --source usb-only
build\Release\UsbCastReceiver.exe --source airplay-only --airplay-name "Conference Display"
build\Release\UsbCastReceiver.exe --source hid-experimental
build\Release\UsbCastReceiver.exe --source hid-experimental --hid-vid 0x1234 --hid-pid 0x5678 --hid-report-size 64
```

`--source auto` is the default and starts the current USB path plus AirPlay DNS-SD discovery when Bonjour is installed. `--source usb-only` keeps the legacy USB-only behavior. `--source airplay-only` skips USB startup and only starts AirPlay discovery scaffolding. `--source hid-experimental` starts the AXTP HID media adapter for parser/flow-control integration tests and is not a default product path. `--no-airplay`, `--no-usb`, `--airplay-name`, and `--airplay-pin` are also supported. If `dnssd.dll` is missing, AirPlay discovery is disabled and USB can still run.

HID AXTP media expectations:

- HID reports are opened and polled through cpp-runtime `axtp_transport_hidapi`; this target owns the hidapi dependency and exposes the required include/link settings.
- HID reports carry AXTP Standard Frame bytes. cpp-runtime strips the configured report ID and feeds AXTP bytes into its protocol parser.
- STREAM payloads use the fixed 16-byte header `streamId:uint32`, `seqId:uint32`, `cursor:uint64`, then opaque `data`; the 16-byte header is parsed by cpp-runtime before this app sees the media chunk.
- The experimental defaults are `streamId=1` for H.264 Annex-B video and `streamId=2` for AAC audio, with `cursor=timestampUs`.
- Video `data` may be the provisional big-endian `VideoChunkHeaderV1` field layout from the draft followed by H.264 bytes; otherwise it is treated as one complete H.264 access unit. Missing video chunks reset frame reassembly and should be followed by a future `video.requestKeyFrame` control action.
- Audio `data` may be the provisional big-endian `AudioChunkHeaderV1` field layout from the draft followed by AAC bytes; otherwise it is treated as one complete AAC chunk. Missing audio chunks are dropped locally without retransmission. The formal chunk-envelope binary layout is still owned by AXTP adoption/generated output.

Video diagnostics:

```bat
build\Release\UsbCastReceiver.exe --uvc-match "Wireless transceiver NA20 "
build\Release\UsbCastReceiver.exe --uvc-match "Wireless transceiver NA20 " --video-fps 25
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --video-backend capture --preview-sink default
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --video-backend capture --video-format auto --preview-sink default
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --video-backend capture --preview-sink add-stream
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --video-backend capture --preview-sink rgb32
build\Release\UsbCastReceiver.exe --video-backend self-test
```

`--video-backend source-reader` is the default and uses Source Reader to request NV12 DXVA output before presenting frames through the D3D11 VideoProcessor and swap chain. `--video-backend capture` uses Media Foundation Capture Engine preview and is kept as an optional compatibility experiment. `--video-backend self-test` does not open any device; it only paints animated color bars into the video HWND to verify Win32 presentation. `--video-format h264` is the default and selects an H.264 native UVC type when present. `--video-format auto` leaves the current device media type untouched and lets the selected backend choose. `--video-fps 25` optionally prefers a native UVC media type whose `MF_MT_FRAME_RATE` is close to 25 fps, forces `25/1` as the D3D11 VideoProcessor frame-rate metadata, and caps Source Reader presentation to 25 fps while still consuming extra samples. If no matching UVC type is found, the app logs the miss, falls back to the previous media-type selection behavior, and still uses the requested renderer metadata and presentation cap. `--preview-sink default` is the default Capture Engine mode and only calls `SetRenderHandle`; `add-stream` and `rgb32` are diagnostic modes for driver stacks that need explicit preview sink configuration.

## Implemented

- Win32 main window with a child video window.
- Right-side icon control rail for mute/unmute, stop, and maximize/restore.
- On-screen render FPS overlay in the upper-left corner for quick smoothness diagnostics.
- ESC restores a maximized window, otherwise closes the application.
- UVC enumeration through Media Foundation device sources.
- UAC enumeration through MMDevice API.
- Source Reader video backend that paints a visible test pattern before frame delivery, then presents decoded NV12 DXVA frames through the D3D11 VideoProcessor.
- Optional Media Foundation Capture Engine preview player.
- WASAPI PCM capture-to-render relay.
- Unified media-core primitives for timestamp mapping, a 3-frame latest-frame video queue, late-frame dropping, future-frame rebasing, and media stats.
- Source/session abstractions for USB, AirPlay, and HID media integration.
- AirPlay DNS-SD discovery service with dynamic `dnssd.dll` loading, `_airplay._tcp` and `_raop._tcp` TXT records, v1 H.264 mirror/audio feature policy, persistent device id fallback, and Bonjour-missing degradation.
- HID experimental AXTP Standard Frame/STREAM adapter for H.264 Annex-B video chunks and AAC audio chunks, including sequence gap detection, 100 ms partial video-frame timeout, timestamp mapping, and encoded media submission into `MediaCore`.
- Thread-safe mute state that keeps consuming capture data.
- OutputDebugStringW logging for device discovery, formats, and HRESULT failures.

## Current Limits

- If the UAC capture format differs from the default render mix format, the relay uses the Windows Audio Resampler DSP through Media Foundation. This covers common PCM/float sample-rate and channel-count differences, such as 16 kHz mono capture to 48 kHz stereo render.
- Some UVC H.264 devices may not preview directly through Capture Engine on every driver stack. Use the default Source Reader backend for those devices.
- The Source Reader renderer now requests DXVA-backed NV12 D3D11 surfaces. If a driver or decoder stack returns system-memory samples instead of `IMFDXGIBuffer`, the app logs that zero-copy is not active and fails the path so the issue is visible.
- AirPlay discovery is present, but AirPlay playback is not complete in this build. RAOP/HTTP/FairPlay, mirror H.264 decode through Media Foundation, AirPlay audio RTP, and A/V sync still need to be migrated before iPhone/macOS mirroring will render.
- HID AXTP media parsing is present, but it currently lands encoded H.264/AAC in `MediaCore`; shared H.264/AAC decode/render plumbing is still part of the broader media-core migration.

## Troubleshooting

- The app writes a UTF-8 diagnostic log on startup. The preferred location is `%LOCALAPPDATA%\UsbCastReceiver\logs\UsbCastReceiver-YYYYMMDD-HHMMSS-PID.log`; if that cannot be opened, it falls back to a `logs` directory beside the executable and then the system temp directory. The first log line also prints the chosen path to Visual Studio Output. If the process exits because of an unhandled exception or `std::terminate`, the last log lines should include that failure.
- For sharp 1080p output, the app enables Per-Monitor DPI awareness and asks the main window to match the native decoded video size. Check the log for `SourceReader D3D11 video processor created: input=1920x1080 ... output=1920x1080 scale=1:1`. If the log says `scale=resample`, the current window or monitor size is still forcing scaling.
- `CoCreateInstance(CLSID_MFCaptureEngine) failed: 0x80004002 (No such interface supported)` means the app could not obtain `IMFCaptureEngine` before opening the UVC device. The code now first tries `IMFCaptureEngineClassFactory::CreateInstance`, then falls back to direct `CLSID_MFCaptureEngine` creation and logs both HRESULT values.
- If Capture Engine creation still fails, verify the machine is a full Windows 10/11 desktop install with Media Foundation components available. Windows N/KN editions may require the Media Feature Pack.
- If Capture Engine creation succeeds but `IMFCaptureEngine::Initialize` fails, investigate device selection, camera privacy settings, device occupation by another process, UVC driver behavior, and supported media types.
- If the window stays blank, first run `--video-backend self-test`. If the animated color bars are not visible, the issue is in the launched binary, window parenting, or Win32 presentation path rather than UVC decode. If self-test works, try `--video-backend source-reader`; it should first paint colored diagnostic bars and then log decoded frames.
- For deeper Media Foundation diagnostics, run:

```bat
mftrace -log mftrace.txt build\Release\UsbCastReceiver.exe --uvc-match "your device"
```

## Performance Principles

- No Electron, Chromium, or WebView render chain.
- Keep the current Source Reader D3D11 path allocation-stable and avoid per-frame CPU pixel copies.
- Prefer Windows Media Foundation system H.264 decoding.
- Use only a few Win32 controls for UI.
- Avoid layered transparent windows to reduce extra DWM composition cost.
- Do not create D3D, Media Foundation, or COM resources per frame.
- Muting writes silence instead of restarting audio devices.
- Keep video and audio lifetimes independent and stoppable.

## TODO

- Add a controlled fallback from NV12/DXVA to RGB32 upload for machines or drivers that cannot provide DXGI-backed decode surfaces.
- Wire the UxPlay RAOP/HTTP/FairPlay protocol layer into `AirPlaySourceAdapter`.
- Add `MfH264Decoder` for AirPlay Annex-B H.264 and connect decoded D3D11 NV12 frames into `MediaCore`.
- Split current Source Reader and WASAPI relay into USB source adapters plus shared D3D11/WASAPI renderers.
- Harden audio resampling with drift handling and glitch metrics.
- Add audio/video sync and latency handling.
- Add FPS, dropped-frame, and end-to-end latency statistics.
- Refine the icon control rail hit testing and visual states if needed.

## Validation Suggestions

- Run 1080p60 playback continuously for 30 minutes.
- Test 4K input if the USB device supports it.
- Monitor CPU and GPU utilization.
- Resize, maximize, and restore the window repeatedly.
- Toggle mute/unmute and listen for pops or glitches.
- Watch memory usage during long playback.
