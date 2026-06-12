# UsbCastReceiver

UsbCastReceiver is a Windows native proof-of-concept for a USB cast receiver device. It avoids Electron, Chromium, WebView, Qt, FFmpeg, ImGui, and third-party dependencies. The goal is to keep the render and audio paths as close as possible to Windows system multimedia components.

## Architecture

Video:

```text
UVC H.264 -> Media Foundation Source Reader -> system H.264 decoder -> RGB32 -> D3D11 texture upload -> DXGI swap chain
```

Audio:

```text
UAC PCM -> WASAPI capture -> WASAPI render
```

The default video path uses Media Foundation Source Reader, asks Windows to decode the UVC H.264 stream to RGB32, uploads each frame into a D3D11 texture, and presents through a DXGI swap chain bound to the child `HWND`. This is the working path for the tested `Wireless transceiver NA20` device. The Capture Engine preview path is still available with `--video-backend capture`, but it is treated as experimental because some UVC driver stacks fail or stay blank even when Source Reader can pull frames.

The audio path captures PCM from the selected UAC endpoint and writes it to the default render endpoint. Muting does not stop or reopen the capture device; the relay keeps draining capture buffers and writes silence to render.

## Build

Use Windows 10/11 x64 with Visual Studio 2022:

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

Video diagnostics:

```bat
build\Release\UsbCastReceiver.exe --uvc-match "Wireless transceiver NA20 "
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --video-backend capture --preview-sink default
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --video-backend capture --video-format auto --preview-sink default
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --video-backend capture --preview-sink add-stream
build\Release\UsbCastReceiver.exe --uvc-match "camera name" --video-backend capture --preview-sink rgb32
build\Release\UsbCastReceiver.exe --video-backend self-test
```

`--video-backend source-reader` is the default and uses Source Reader to decode to RGB32 before presenting frames through the current D3D11 swap-chain renderer. `--video-backend capture` uses Media Foundation Capture Engine preview and is kept as an optional compatibility experiment. `--video-backend self-test` does not open any device; it only paints animated color bars into the video HWND to verify Win32 presentation. `--video-format h264` is the default and selects an H.264 native UVC type when present. `--video-format auto` leaves the current device media type untouched and lets the selected backend choose. `--preview-sink default` is the default Capture Engine mode and only calls `SetRenderHandle`; `add-stream` and `rgb32` are diagnostic modes for driver stacks that need explicit preview sink configuration.

## Implemented

- Win32 main window with a child video window.
- Right-side icon control rail for mute/unmute, stop, and maximize/restore.
- ESC restores a maximized window, otherwise closes the application.
- UVC enumeration through Media Foundation device sources.
- UAC enumeration through MMDevice API.
- Source Reader video backend that paints a visible test pattern before frame delivery, then presents decoded RGB32 frames through D3D11.
- Optional Media Foundation Capture Engine preview player.
- WASAPI PCM capture-to-render relay.
- Thread-safe mute state that keeps consuming capture data.
- OutputDebugStringW logging for device discovery, formats, and HRESULT failures.

## Current Limits

- If the UAC capture format differs from the default render mix format, the relay uses the Windows Audio Resampler DSP through Media Foundation. This covers common PCM/float sample-rate and channel-count differences, such as 16 kHz mono capture to 48 kHz stereo render.
- Some UVC H.264 devices may not preview directly through Capture Engine on every driver stack. Use the default Source Reader backend for those devices.
- The current Source Reader renderer still receives CPU-visible RGB32 samples and uploads them into a D3D11 texture per frame. The next performance step is DXVA/D3D-backed decode output, preferably NV12 GPU textures, to remove the CPU RGB32 copy/upload path.

## Troubleshooting

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
- Keep the current Source Reader D3D11 path allocation-stable; the next renderer step should remove the CPU RGB32 upload from the steady-state performance path.
- Prefer Windows Media Foundation system H.264 decoding.
- Use only a few Win32 controls for UI.
- Avoid layered transparent windows to reduce extra DWM composition cost.
- Do not create D3D, Media Foundation, or COM resources per frame.
- Muting writes silence instead of restarting audio devices.
- Keep video and audio lifetimes independent and stoppable.

## TODO

- Replace the Source Reader RGB32 texture upload path with DXVA/D3D-backed NV12 decode textures.
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
