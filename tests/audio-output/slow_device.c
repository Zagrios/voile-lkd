/*
 * slow_device.c - makes CoreAudio's output behave like a device that is not ready.
 *
 * Inserted into wsd by run.sh (DYLD_INSERT_LIBRARIES):
 *   WSD_TEST_START_DELAY_MS  AudioOutputUnitStart first waits that long, so a unit renders its first frames that much
 *                            later, as it does on a Bluetooth headset.
 *   WSD_TEST_STALL_MS        a unit stops asking wsd for frames for that long, one second after its first render, as
 *                            it does while the Mac changes its output device.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t env_ns(const char *name) {
  const char *ms = getenv(name);
  return ms ? (uint64_t)atoi(ms) * 1000000 : 0;
}

static OSStatus slow_start(AudioUnit unit) {
  usleep((useconds_t)(env_ns("WSD_TEST_START_DELAY_MS") / 1000));
  return AudioOutputUnitStart(unit);
}

struct render { AURenderCallbackStruct wsd; uint64_t first_ns, stall_ns; };

static OSStatus stalling_render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                                UInt32 frames, AudioBufferList *io) {
  struct render *r = ref;
  uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  if (!r->first_ns) r->first_ns = now;
  if (now - r->first_ns < 1000000000 || now - r->first_ns >= 1000000000 + r->stall_ns)
    return r->wsd.inputProc(r->wsd.inputProcRefCon, flags, ts, bus, frames, io);
  for (UInt32 i = 0; i < io->mNumberBuffers; i++) memset(io->mBuffers[i].mData, 0, io->mBuffers[i].mDataByteSize);
  return noErr;
}

static OSStatus set_property(AudioUnit unit, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element,
                             const void *data, UInt32 size) {
  uint64_t stall_ns = env_ns("WSD_TEST_STALL_MS");
  if (id == kAudioUnitProperty_SetRenderCallback && stall_ns && size == sizeof(AURenderCallbackStruct)) {
    struct render *r = calloc(1, sizeof(*r)); /* lives as long as wsd: a unit may render until it is disposed */
    r->wsd = *(const AURenderCallbackStruct *)data;
    r->stall_ns = stall_ns;
    AURenderCallbackStruct cb = {stalling_render, r};
    return AudioUnitSetProperty(unit, id, scope, element, &cb, sizeof(cb));
  }
  return AudioUnitSetProperty(unit, id, scope, element, data, size);
}

#define INTERPOSE(replacement, replacee) \
  __attribute__((used)) static const struct { const void *with, *what; } interpose_##replacee \
      __attribute__((section("__DATA,__interpose"))) = {(const void *)replacement, (const void *)replacee}
INTERPOSE(slow_start, AudioOutputUnitStart);
INTERPOSE(set_property, AudioUnitSetProperty);
