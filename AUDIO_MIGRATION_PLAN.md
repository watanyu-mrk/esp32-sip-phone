# Audio migration plan (Phase 3)

Status (2026-11-12): the migration is **implemented**, not yet validated on hardware.

- `SIP_PHONE_AUDIO_INMP441` (default) compiles `audio_i2s_input.c` + `audio_i2s_output.c`; the XIAO PDM/I2S sources are only compiled when `SIP_PHONE_AUDIO_XIAO_PDM` is selected, and `SIP_PHONE_AUDIO_NONE` builds no audio driver at all.
- Nothing has been heard or recorded from a real board yet: the INMP441 is not wired and the amplifier pins below are still unconfirmed. Do not treat calls on this board as having working audio until the validation steps at the end of this document pass.

## Target hardware

| Direction | Device | Wiring |
| --- | --- | --- |
| Capture | INMP441 I2S MEMS mic | VDD 3.3 V, GND, SCK=GPIO6, WS=GPIO7, SD=GPIO15, L/R=GND (left channel) |
| Playback | Onboard amplifier + speaker | I2S TX; pins to be confirmed from the schematic (brief proposes BCLK=42, LRC=2, DOUT=41) |

The amplifier's enable/mute pin (if any) and its part number are not documented in the sources found. Phase 3 starts by confirming the schematic before writing the driver.

## Design

1. **Two I2S peripherals, not one duplex port.** ESP32-S3 has two user I2S controllers. INMP441 runs on I2S0 in RX (Philips, 16 kHz or 8 kHz, 16/24-bit), the amplifier on I2S1 in TX (8 kHz mono 16-bit). Keeping them separate avoids the half-duplex limitations of a single standard-mode port and matches the existing two-task structure.
2. **Keep the 8 kHz PCM contract.** `audio_driver.h` already defines `AUDIO_SAMPLE_RATE_HZ=8000`, 20 ms frames, `audio_read_pcm()` / `audio_write_pcm()`. RTP, jitter buffer and `phone_control` stay untouched.
3. **INMP441 specifics.** 24-bit samples in 32-bit slots (or 16-bit slots as a fallback), left channel selected by tying L/R to GND. Convert to 16-bit PCM with a shift and clamp; do not assume the mic is 16-bit native. Reuse the existing DC tracker and software gain (`mic_gain_db`).
4. **Speaker specifics.** I2S Philips mode, 8 kHz, mono, 16-bit, DMA ring of at least 4 × 20 ms. Apply `speaker_volume` in software before the write. Drive the amplifier enable pin high only while a call or ringtone is active if the hardware has one.
5. **Full duplex.** Capture and playback stay in separate tasks with stream buffers, as today. Bounded waits (≤ 50 ms) on every I2S call; no allocation in the audio path.
6. **Mute.** Mute must stop sending microphone samples (send silence or skip the send) while RTP receive and playback continue. DTMF and ringtone generation ride the playback path.
7. **AEC/noise reduction.** Only if ESP-SR fits in the 4 MB flash and PSRAM budget; otherwise document that echo cancellation is absent. Basic high-pass/DC removal is already in the capture path.

## Validation (in order)

1. Mic-only loop: log RMS/peak dBFS at 1 Hz, confirm speech moves the meter and silence does not.
2. Speaker-only loop: play a 1 kHz tone and a swept sine; confirm no underruns and audible output.
3. Full duplex with a local call: speak into the INMP441, verify RTP send counters and received audio on a PC UAC.
4. Real PBX call: register as `901`, place and answer a call, check `sent/recv/lost` and subjective quality.
5. Long-call soak (5 minutes) and repeated call setup/teardown for leaks.

Each step must be recorded with the actual serial output; none of it is claimed until it runs on a JC4827W543.
