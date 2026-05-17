/***************************************************************************
 *   Copyright (C) 2019 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#pragma once

#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

#include "core/decode_xa.h"
#include "core/spu.h"
#include "core/sstate.h"
#include "json.hpp"
#include "spu/adsr.h"
#include "spu/event_capture.h"
#include "spu/miniaudio.h"
#include "spu/types.h"
#include "support/settings.h"

namespace PCSX {

namespace SPU {

class impl final : public SPUInterface {
  public:
    using json = nlohmann::json;
    bool open() final;
    // SPU Functions
    long init(void) final;
    long shutdown(void) final;
    long close(void) final;
    void wipeChannels();
    // void playSample(uint8_t);
    void writeRegister(uint32_t, uint16_t) final;
    uint16_t readRegister(uint32_t) final;
    // Inner body of readRegister; the public entry point wraps this so the
    // event-stream capture (fft-project) sees the returned value.
    uint16_t readRegisterInner(uint32_t);
    void lockSPURAM() final;
    void unlockSPURAM() final;
    void resetCaptureBuffer() final;
    void writeDMAMem(uint16_t *, int) final;
    void readDMAMem(uint16_t *, int) final;
    virtual void playADPCMchannel(xa_decode_t *) final;

    void save(SaveStates::SPU &) final;
    void load(const SaveStates::SPU &) final;

    virtual void setLua(Lua L) override;

    void async(uint32_t) final;
    void playCDDAchannel(int16_t *, int) final;
    void registerCDDAVolume(void (*CDDAVcallback)(uint16_t, uint16_t));

    // num of channels
    static const size_t MAXCHAN = 24;
    // number of characters for a channel tag
    static constexpr unsigned CHANNEL_TAG = 32;
    // number of samples for debugger wave plot
    static const unsigned DEBUG_SAMPLES = 1024;

    uint32_t getFrameCount() override { return m_audioOut.getFrameCount(); }

    void debug() final;
    bool configure() final;
    json getCfg() final { return settings.serialize(); }
    void setCfg(const json &j) final {
        if (j.count("SPU") && j["SPU"].is_object()) {
            settings.deserialize(j["SPU"]);
        } else {
            settings.reset();
        }
    }
    uint32_t getCurrentFrames() override { return m_audioOut.getCurrentFrames(); }
    void waitForGoal(uint32_t goal) override { m_audioOut.waitForGoal(goal); }

  private:
    struct ADSRFlags {
        enum : uint16_t {
            AttackMode = 1 << 15,      // 15 0=Linear, 1=Exponential
            AttackShiftMask = 0x7c00,  // 14-10 0..1Fh = Fast..Slow
            AttackStepMask = 0x300,    // 9-8 0..3 = "+7,+6,+5,+4"
            DecayShiftMask = 0xf0,     // 7-4 0..0Fh = Fast..Slow
            SustainLevelMask = 0xf,    // 3-0 0..0Fh  ;Level=(N+1)*800h
            // Flags for upper 16-bits of reg, shifted right 16-bits
            SustainMode = 1 << 15,       // 31 0=Linear, 1=Exponential
            SustainDirection = 1 << 14,  // 30  0=Increase, 1=Decrease (until Key OFF flag)
            SustainShiftMask = 0x1f00,   // 28-24 0..1Fh = Fast..Slow
            SustainStepMask = 0xc0,      // 23-22 0..3 = "+7,+6,+5,+4" or "-8,-7,-6,-5") (inc/dec)
            ReleaseMode = 1 << 5,        // 21 0=Linear, 1=Exponential
            ReleaseShiftMask = 0x1f      // 20-16 0..1Fh = Fast..Slow
        };
    };

    struct ControlFlags {
        enum : uint16_t {
            CDAudioEnable = 1 << 0,         // 0 0=Off, 1=On (for CD-DA and XA-ADPCM)
            ExternalAudioEnable = 1 << 1,   // 1 0=Off, 1=On
            CDReverbEnable = 1 << 2,        // 20=Off, 1=On (for CD-DA and XA-ADPCM)
            ExternalReverbEnable = 1 << 3,  // 3 0=Off, 1=On
            RAMTransferModeMask = 0x0030,   // 5-4 0=Stop, 1=ManualWrite, 2=DMAwrite, 3=DMAread
            IRQEnable = 1 << 6,             // 6 0=Disabled/Acknowledge, 1=Enabled; only when Bit15=1
            ReverbMasterEnable = 1 << 7,    // 7 0=Disabled, 1=Enabled
            NoiseStepMask = 0x0300,         // 9-8 0..03h = Step "4,5,6,7"
            NoiseShiftMask = 0x3c00,        // 13-10 0..0Fh = Low .. High Frequency
            Mute = 1 << 14,                 // 14 0=Mute, 1=Unmute
            Enable = 1 << 15                // 15 0=Off, 1=On
        };
    };

    struct StatusFlags {
        enum : uint16_t {
            SPUModeMask = 0x3f,        // 5-0 Current SPU Mode(same as SPUCNT.Bit5 - 0, but, applied a bit delayed)
            IRQFlag = 1 << 6,          // 6 IRQ9 Flag (0=No, 1=Interrupt Request)
            DMARWRequest = 1 << 7,     // 7 Data Transfer DMA Read/Write Request seems to be same as SPUCNT.Bit5
            DMAWriteRequest = 1 << 8,  // 8 Data Transfer DMA Write Request (0=No, 1=Yes)
            DMAReadRequest = 1 << 9,   // 9 Data Transfer DMA Read Request (0=No, 1=Yes)
            DMABusy = 1 << 10,         // 10 Data Transfer Busy Flag (0=Ready, 1=Busy)
            CBIndex = 11 << 11,        // 11 Writing to First/Second half of Capture Buffers (0=First, 1=Second)
            // 15-12 Unknown/Unused (seems to be usually zero)
        };
    };

    struct VolumeFlags {
        enum : uint16_t {
            VolumeMode = 1 << 15,      // 15 1=Sweep Mode
            SweepMode = 1 << 14,       // 14 0=Linear, 1=Exponential
            SweepDirection = 1 << 13,  // 13 0=Increase, 1=Decrease
            SweepPhase = 1 << 12,      // 12 0=Positive, 1=Negative
            Unknown = 0xf80,           // 7-11 Not used? (should be zero)
            SweepShiftMask = 0x7c,     // 6-2 0..1Fh = Fast..Slow
            SweepStepMask = 0x3        // 1-0 0..3 = "+7,+6,+5,+4" or "-8,-7,-6,-5") (inc/dec)

        };
    };

    // sound buffer sizes
    // 400 ms complete sound buffer
    static const size_t SOUNDSIZE = 70560;
    // 137 ms test buffer... if less than that is buffered, a new upload will happen
    static const size_t TESTSIZE = 24192;

    // ~ 1 ms of data
    static const size_t NSSIZE = 45;

    // spu
    void MainThread();
    void writeCaptureBufferCD(int numbSamples);
    void SetupStreams();
    void RemoveStreams();
    void SetupThread();
    void RemoveThread();
    void StartSound(SPUCHAN *pChannel);
    void VoiceChangeFrequency(SPUCHAN *pChannel);
    void FModChangeFrequency(SPUCHAN *pChannel, int ns);
    int iGetNoiseVal(SPUCHAN *pChannel);
    void StoreInterpolationVal(SPUCHAN *pChannel, int fa);
    int iGetInterpolationVal(SPUCHAN *pChannel);
    void NoiseClock();

    // registers
    void SoundOn(int start, int end, uint16_t val);
    void SoundOff(int start, int end, uint16_t val);
    void FModOn(int start, int end, uint16_t val);
    void NoiseOn(int start, int end, uint16_t val);
    void SetVolumeL(uint8_t ch, int16_t vol);
    void SetVolumeR(uint8_t ch, int16_t vol);
    void SetPitch(int ch, uint16_t val);
    void ReverbOn(int start, int end, uint16_t val);

    // reverb
    int g_buffer(int iOff);              // get_buffer content helper: takes care about wraps
    void s_buffer(int iOff, int iVal);   // set_buffer content helper: takes care about wraps and clipping
    void s_buffer1(int iOff, int iVal);  // set_buffer (+1 sample) content helper: takes care about wraps and clipping
    void InitREVERB();
    void SetREVERB(uint16_t val);
    void StartREVERB(SPUCHAN *pChannel);
    void StoreREVERB(SPUCHAN *pChannel, int ns);
    int MixREVERBLeft(int ns);
    int MixREVERBRight();

    // xa
    void FeedXA(xa_decode_t *xap);

    int bSPUIsOpen;

    // psx buffer / addresses
    uint16_t regArea[10000];
    // Note that SPU ram is a uint16_t, so total size is 512KB.
    uint16_t spuMem[256 * 1024];
    uint8_t *spuMemC;
    uint8_t *pSpuIrq = 0;
    uint8_t *pSpuBuffer;
    uint8_t *pMixIrq = 0;

    struct CaptureBuffer {
        static const int CB_SIZE = 1024 * 16;
        // These buffers have to be large enough to allow the CD-XA to stream in enough data.
        uint16_t CDCapLeft[CB_SIZE] = {0};
        uint16_t CDCapRight[CB_SIZE] = {0};

        int32_t startIndex = 0;
        int32_t endIndex = 0;
        int32_t currIndex = 0;
    };
    std::mutex cbMtx;

    // The temporary cap buffer for CD Audio left/right.
    CaptureBuffer captureBuffer;
    // The cap buffer index for voice 1 and voice 3.
    int32_t capBufVoiceIndex = 0;

    // user settings
    SettingsType settings;

    // MAIN infos struct for each channel

    SPUCHAN s_chan[MAXCHAN + 1];  // channel + 1 infos (1 is security for fmod handling)
    REVERBInfo rvb;

    uint32_t m_noiseClock = 0;  // global noise generator
    uint32_t m_noiseCount = 0;  // global noise generator
    uint32_t m_noiseVal = 1;    // global noise generator

    uint16_t spuCtrl = 0;  // some vars to store psx reg infos
    uint16_t spuStat = 0;
    uint16_t spuIrq = 0;
    uint32_t spuAddr = 0xffffffff;  // address into spu mem
    int bEndThread = 0;             // thread handlers
    int bThreadEnded = 0;
    int bSpuInit = 0;

    std::thread hMainThread;
    uint32_t dwNewChannel = 0;  // flags for faster testing, if new channel starts

    void (*cddavCallback)(uint16_t, uint16_t) = 0;

    // certain globals (were local before, but with the new timeproc I need em global)

    const int f[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    int SSumR[NSSIZE];
    int SSumL[NSSIZE];
    int iFMod[NSSIZE];
    int iCycle = 0;
    int16_t *pS;

    int lastch = -1;       // last channel processed on spu irq in timer mode
    int lastns = 0;        // last ns pos
    int iSecureStart = 0;  // secure start counter
    int iSpuAsyncWait = 0;

    // REVERB info and timing vars...

    int *sRVBPlay = 0;
    int *sRVBEnd = 0;
    int *sRVBStart = 0;
    int iReverbOff = -1;  // some delay factor for reverb
    int iReverbRepeat = 0;
    int iReverbNum = 1;

    // XA
    xa_decode_t *xapGlobal = 0;

    int iLeftXAVol = 32767;
    int iRightXAVol = 32767;

    int gauss_ptr = 0;
    int gauss_window[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    int &gvall0() { return gauss_window[gauss_ptr]; }
    int &gvall(int pos) { return gauss_window[(gauss_ptr + pos) & 3]; }
    int &gvalr0() { return gauss_window[4 + gauss_ptr]; }
    int &gvalr(int pos) { return gauss_window[4 + ((gauss_ptr + pos) & 3)]; }

    ADSR m_adsr;
    MiniAudio m_audioOut = {settings};
    xa_decode_t m_cdda;

    // ===== SPU Audio Capture System =====
    struct SpuCapture {
        std::atomic<bool> recording{false};
        bool capturePerVoice = false;
        std::string outputDir;
        int debugVoice = -1;

        std::ofstream mixFile;
        std::ofstream voiceFiles[MAXCHAN];
        std::ofstream traceFile;
        std::ofstream denseTraceFile;
        std::ofstream blockTraceFile;
        uint32_t sampleCount = 0;

        struct VoiceEventState {
            bool valid = false;
            bool on = false;
            bool stop = false;
            uint16_t rawPitch = 0;
            uint32_t startAddr = 0;
            uint32_t loopAddr = 0;
            uint32_t currAddr = 0;
            int envState = -1;
            int envVol = -1;
            uint16_t adsr1 = 0xffff;  // 0xffff so first event always fires
            uint16_t adsr2 = 0xffff;
        };

        VoiceEventState prevVoiceEvents[MAXCHAN];

        void start(const std::string& dir, bool perVoice = false, int debugVoiceIndex = -1) {
            outputDir = dir;
            capturePerVoice = perVoice;
            debugVoice = debugVoiceIndex;
            sampleCount = 0;
            for (unsigned i = 0; i < MAXCHAN; i++) {
                prevVoiceEvents[i] = VoiceEventState{};
            }

            mixFile.open(dir + "/spu_mix.raw", std::ios::binary);
            traceFile.open(dir + "/spu_voice_events.jsonl", std::ios::binary);
            if (debugVoice >= 0 && debugVoice < static_cast<int>(MAXCHAN)) {
                denseTraceFile.open(dir + "/spu_voice_dense.jsonl", std::ios::binary);
                blockTraceFile.open(dir + "/spu_voice_blocks.jsonl", std::ios::binary);
            }

            if (perVoice) {
                for (unsigned i = 0; i < MAXCHAN; i++) {
                    char name[64];
                    snprintf(name, sizeof(name), "/spu_voice_%02u.raw", i);
                    voiceFiles[i].open(dir + name, std::ios::binary);
                }
            }
            recording = true;
        }

        void stop() {
            recording = false;

            if (traceFile.is_open()) {
                traceFile.close();
            }
            if (denseTraceFile.is_open()) {
                denseTraceFile.close();
            }
            if (blockTraceFile.is_open()) {
                blockTraceFile.close();
            }
            if (mixFile.is_open()) {
                mixFile.close();
                rawToWav(outputDir + "/spu_mix.raw", outputDir + "/spu_mix.wav", 2, sampleCount);
            }
            if (capturePerVoice) {
                for (unsigned i = 0; i < MAXCHAN; i++) {
                    if (voiceFiles[i].is_open()) {
                        voiceFiles[i].close();
                        char rawN[64], wavN[64];
                        snprintf(rawN, sizeof(rawN), "/spu_voice_%02u.raw", i);
                        snprintf(wavN, sizeof(wavN), "/spu_voice_%02u.wav", i);
                        rawToWav(outputDir + rawN, outputDir + wavN, 1, sampleCount);
                    }
                }
            }
        }

        void writeVoiceEvent(unsigned ch, uint32_t sampleIndex, bool on, bool stop, uint16_t rawPitch,
                             uint32_t startAddr, uint32_t loopAddr, uint32_t currAddr,
                             int envState, int envVol, int16_t sample,
                             uint16_t adsr1 = 0, uint16_t adsr2 = 0) {
            if (!recording || !traceFile.is_open() || ch >= MAXCHAN) return;
            auto &prev = prevVoiceEvents[ch];
            if (prev.valid && prev.on == on && prev.stop == stop && prev.rawPitch == rawPitch &&
                prev.startAddr == startAddr && prev.loopAddr == loopAddr && prev.currAddr == currAddr &&
                prev.envState == envState && prev.envVol == envVol &&
                prev.adsr1 == adsr1 && prev.adsr2 == adsr2) {
                return;
            }
            traceFile << "{\"kind\":\"voice_event\",\"voice\":" << ch
                      << ",\"sample_index\":" << sampleIndex
                      << ",\"on\":" << (on ? "true" : "false")
                      << ",\"stop\":" << (stop ? "true" : "false")
                      << ",\"raw_pitch\":" << rawPitch
                      << ",\"start_addr\":" << startAddr
                      << ",\"loop_addr\":" << loopAddr
                      << ",\"curr_addr\":" << currAddr
                      << ",\"env_state\":" << envState
                      << ",\"env_vol\":" << envVol
                      << ",\"sample\":" << sample
                      << ",\"adsr1\":" << adsr1
                      << ",\"adsr2\":" << adsr2
                      << "}\n";
            traceFile.flush();
            prev.valid = true;
            prev.on = on;
            prev.stop = stop;
            prev.rawPitch = rawPitch;
            prev.startAddr = startAddr;
            prev.loopAddr = loopAddr;
            prev.currAddr = currAddr;
            prev.envState = envState;
            prev.envVol = envVol;
            prev.adsr1 = adsr1;
            prev.adsr2 = adsr2;
        }

        void writeVoiceDenseEvent(unsigned ch, uint32_t sampleIndex, int decodedSample, int interpSample, int mixedSample,
                                  uint32_t startAddr, uint32_t loopAddr, uint32_t currAddr,
                                  int envState, int envVol, int sbPos, uint32_t spos, int s1, int s2,
                                  int sincVal, int rawPitch, int actFreq, int usedFreq, int fmod, int noise) {
            if (!recording || !denseTraceFile.is_open()) return;
            if (static_cast<int>(ch) != debugVoice) return;
            denseTraceFile << "{\"kind\":\"voice_dense\""
                           << ",\"voice\":" << ch
                           << ",\"sample_index\":" << sampleIndex
                           << ",\"decoded_sample\":" << decodedSample
                           << ",\"interp_sample\":" << interpSample
                           << ",\"mixed_sample\":" << mixedSample
                           << ",\"start_addr\":" << startAddr
                           << ",\"loop_addr\":" << loopAddr
                           << ",\"curr_addr\":" << currAddr
                           << ",\"env_state\":" << envState
                           << ",\"env_vol\":" << envVol
                           << ",\"sb_pos\":" << sbPos
                           << ",\"spos\":" << spos
                           << ",\"s_1\":" << s1
                           << ",\"s_2\":" << s2
                           << ",\"sinc\":" << sincVal
                           << ",\"raw_pitch\":" << rawPitch
                           << ",\"act_freq\":" << actFreq
                           << ",\"used_freq\":" << usedFreq
                           << ",\"fmod\":" << fmod
                           << ",\"noise\":" << noise
                           << "}\n";
        }

        void writeVoiceBlock(unsigned ch, uint32_t sampleIndex, uint32_t blockAddr, const uint8_t *blockData,
                             int inS1, int inS2, int predictNr, int shiftFactor, int flags) {
            if (!recording || !blockTraceFile.is_open()) return;
            if (static_cast<int>(ch) != debugVoice) return;
            blockTraceFile << "{\"kind\":\"voice_block\""
                           << ",\"voice\":" << ch
                           << ",\"sample_index\":" << sampleIndex
                           << ",\"block_addr\":" << blockAddr
                           << ",\"predict\":" << predictNr
                           << ",\"shift\":" << shiftFactor
                           << ",\"flags\":" << flags
                           << ",\"in_s1\":" << inS1
                           << ",\"in_s2\":" << inS2
                           << ",\"bytes\":[";
            for (int i = 0; i < 16; i++) {
                if (i != 0) blockTraceFile << ",";
                blockTraceFile << static_cast<unsigned>(blockData[i]);
            }
            blockTraceFile << "]}\n";
        }

        void writeMix(int16_t left, int16_t right) {
            if (!recording || !mixFile.is_open()) return;
            mixFile.write(reinterpret_cast<char*>(&left), 2);
            mixFile.write(reinterpret_cast<char*>(&right), 2);
            sampleCount++;
        }

        void writeVoice(unsigned ch, int16_t sample) {
            if (!recording || !capturePerVoice || ch >= MAXCHAN) return;
            if (!voiceFiles[ch].is_open()) return;
            voiceFiles[ch].write(reinterpret_cast<char*>(&sample), 2);
        }

        static void rawToWav(const std::string& rawPath, const std::string& wavPath,
                             uint16_t channels, uint32_t numSamples) {
            std::ifstream raw(rawPath, std::ios::binary);
            if (!raw.is_open()) return;
            raw.seekg(0, std::ios::end);
            uint32_t dataSize = (uint32_t)raw.tellg();
            raw.seekg(0, std::ios::beg);
            std::vector<char> data(dataSize);
            raw.read(data.data(), dataSize);
            raw.close();

            std::ofstream wav(wavPath, std::ios::binary);
            uint32_t sampleRate = 44100;
            uint16_t bitsPerSample = 16;
            uint16_t blockAlign = channels * bitsPerSample / 8;
            uint32_t byteRate = sampleRate * blockAlign;
            uint32_t fileSize = 36 + dataSize;
            uint16_t audioFmt = 1;
            uint32_t fmtSize = 16;

            wav.write("RIFF", 4);
            wav.write(reinterpret_cast<char*>(&fileSize), 4);
            wav.write("WAVE", 4);
            wav.write("fmt ", 4);
            wav.write(reinterpret_cast<char*>(&fmtSize), 4);
            wav.write(reinterpret_cast<char*>(&audioFmt), 2);
            wav.write(reinterpret_cast<char*>(&channels), 2);
            wav.write(reinterpret_cast<char*>(&sampleRate), 4);
            wav.write(reinterpret_cast<char*>(&byteRate), 4);
            wav.write(reinterpret_cast<char*>(&blockAlign), 2);
            wav.write(reinterpret_cast<char*>(&bitsPerSample), 2);
            wav.write("data", 4);
            wav.write(reinterpret_cast<char*>(&dataSize), 4);
            wav.write(data.data(), dataSize);
            wav.close();

            std::remove(rawPath.c_str());
        }
    } m_capture;

    // Event-stream capture: records every input to the SPU (reg writes/reads,
    // DMA, XA) into a binary log + initial-state snapshot. See event_capture.h.
    // Trigger via Lua: PCSX.SPU.startEventCapture(path, label) / stopEventCapture().
    SpuEventCapture m_eventCapture;

    // debug window
    unsigned m_selectedChannel = 0;
    std::chrono::time_point<std::chrono::steady_clock> m_lastUpdated;
    enum { EMPTY = 0, DATA, NOISE, FMOD1, FMOD2, IRQ, MUTED } m_channelDebugTypes[MAXCHAN][DEBUG_SAMPLES];
    float m_channelDebugData[MAXCHAN][DEBUG_SAMPLES];
    char m_channelTag[MAXCHAN][CHANNEL_TAG] = {};
    unsigned m_currentDebugSample = 0;
};

}  // namespace SPU

}  // namespace PCSX
