// SPU event-stream capture (fft-project).
//
// Records every input that reaches the SPU (register writes/reads, DMA
// payloads, CD-XA frames) into a binary log, plus a one-shot snapshot of
// SPU register state + 512 KiB SPU RAM at capture start. The log is the
// portable contract for offline replay through alternative SPU
// implementations.
//
// Format: see comments below. v1 = uncompressed, sync writes.

#pragma once

#include <stdint.h>

#include <atomic>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <string>

namespace PCSX {
namespace SPU {

namespace EventStream {

static constexpr char     kMagic[8]   = {'S','P','U','S','T','R','M','\0'};
static constexpr uint32_t kVersion    = 1;
static constexpr size_t   kFileHeaderSize    = 72;
static constexpr size_t   kRegCount          = 0x200;     // 512 × u16 → 0x1F801C00–0x1F801FFF
static constexpr size_t   kInitialRamBytes   = 512 * 1024;
static constexpr size_t   kInitialStateSize  = kRegCount * sizeof(uint16_t) + kInitialRamBytes;

enum Kind : uint8_t {
    REG_WRITE   = 0x01,
    REG_READ    = 0x02,
    DMA_IN      = 0x03,
    DMA_OUT     = 0x04,
    IRQ_ACK     = 0x05,
    SAMPLE_MARK = 0x06,
    XA_IN       = 0x10,
};

#pragma pack(push, 1)
struct FileHeader {
    char     magic[8];               // kMagic
    uint32_t version;                // kVersion
    uint32_t flags;                  // reserved; bit 0 = payload_lz4 (not used in v1)
    uint64_t total_events;           // # event records following initial state
    uint64_t initial_state_size;     // bytes — InitialState section length
    uint64_t cycles_total;           // wall-clock equiv at end of capture
    uint64_t spu_samples_total;      // # of stereo samples produced (sanity); 0 if unknown
    char     session_label[24];      // human label, e.g. "protect_no_music"
};
static_assert(sizeof(FileHeader) == kFileHeaderSize, "FileHeader must be 64 bytes");

struct EventRecord {
    uint64_t cycle;       // PSX CPU cycle when this event occurred
    uint32_t length;      // payload length in bytes (0 if none); 32-bit so DMA bursts >64KB fit
    uint8_t  kind;        // Kind enum
    uint8_t  pad;         // reserved
    uint16_t reserved;    // reserved for future flags
    // payload bytes follow (variable)
};
static_assert(sizeof(EventRecord) == 16, "EventRecord header must be 16 bytes");
#pragma pack(pop)

}  // namespace EventStream

// SpuEventCapture lives inside `impl` (see interface.h). Methods are
// called from the SPU register/DMA/XA paths inside the emulator thread.
// Writes are serialized under a mutex.
class SpuEventCapture {
  public:
    bool start(const std::string& output_path, const std::string& session_label);

    // Caller must invoke set_initial_state() *exactly once* right after
    // start(), before any emit_* call. We hold the header slot for it
    // and only begin event records after.
    void set_initial_state(const uint16_t* spu_regs, const uint8_t* spu_ram);

    void stop();
    bool is_recording() const { return m_recording.load(); }
    uint64_t event_count() const { return m_event_count; }

    void emit_reg_write(uint16_t offset, uint16_t value);
    void emit_reg_read (uint16_t offset, uint16_t returned);
    void emit_dma_in   (const uint8_t* data, uint32_t bytes);
    void emit_dma_out  (const uint8_t* data, uint32_t bytes);
    void emit_xa       (const uint8_t* data, uint32_t bytes);

    // Optional debug anchor; called by mixer at sample-mark intervals.
    void emit_sample_mark(uint64_t sample_idx);

  private:
    uint64_t current_cycle();
    void write_event(uint8_t kind, const void* payload, uint16_t length);

    std::atomic<bool> m_recording{false};
    std::mutex        m_mtx;
    std::ofstream     m_file;
    std::string       m_path;
    std::string       m_session_label;
    bool              m_initial_state_written = false;
    uint64_t          m_event_count = 0;
};

}  // namespace SPU
}  // namespace PCSX
