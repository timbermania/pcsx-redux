// SPU event-stream capture — implementation. See event_capture.h.

#include "spu/event_capture.h"

#include <cstring>

#include "core/psxemulator.h"
#include "core/r3000a.h"

namespace PCSX {
namespace SPU {

uint64_t SpuEventCapture::current_cycle() {
    // PCSX::g_emulator->m_cpu->m_regs.cycle is the global CPU cycle
    // counter (uint64_t); see vendor/pcsx-redux/src/core/r3000a.h.
    if (!PCSX::g_emulator || !PCSX::g_emulator->m_cpu) return 0;
    return PCSX::g_emulator->m_cpu->m_regs.cycle;
}

bool SpuEventCapture::start(const std::string& output_path,
                            const std::string& session_label) {
    if (m_recording.load()) return false;

    std::lock_guard<std::mutex> lk(m_mtx);
    m_file.open(output_path, std::ios::binary | std::ios::trunc);
    if (!m_file.is_open()) return false;

    m_path = output_path;
    m_session_label = session_label;
    m_initial_state_written = false;
    m_event_count = 0;

    // Reserve header space; we'll seek back and rewrite on stop().
    EventStream::FileHeader hdr{};
    std::memcpy(hdr.magic, EventStream::kMagic, 8);
    hdr.version = EventStream::kVersion;
    hdr.flags = 0;
    hdr.total_events = 0;
    hdr.initial_state_size = EventStream::kInitialStateSize;
    hdr.cycles_total = 0;
    hdr.spu_samples_total = 0;
    std::memset(hdr.session_label, 0, sizeof(hdr.session_label));
    std::strncpy(hdr.session_label, session_label.c_str(),
                 sizeof(hdr.session_label) - 1);
    m_file.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    m_recording.store(true);
    return true;
}

void SpuEventCapture::set_initial_state(const uint16_t* spu_regs,
                                        const uint8_t* spu_ram) {
    if (!m_recording.load() || m_initial_state_written) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_file.is_open()) return;

    m_file.write(reinterpret_cast<const char*>(spu_regs),
                 EventStream::kRegCount * sizeof(uint16_t));
    m_file.write(reinterpret_cast<const char*>(spu_ram),
                 EventStream::kInitialRamBytes);
    m_initial_state_written = true;
}

void SpuEventCapture::stop() {
    if (!m_recording.load()) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_file.is_open()) {
        m_recording.store(false);
        return;
    }

    // Backpatch the FileHeader with final counts.
    uint64_t final_cycle = current_cycle();
    m_file.flush();
    m_file.seekp(0, std::ios::beg);

    EventStream::FileHeader hdr{};
    std::memcpy(hdr.magic, EventStream::kMagic, 8);
    hdr.version = EventStream::kVersion;
    hdr.flags = 0;
    hdr.total_events = m_event_count;
    hdr.initial_state_size = m_initial_state_written
        ? EventStream::kInitialStateSize : 0;
    hdr.cycles_total = final_cycle;
    hdr.spu_samples_total = 0;
    std::memset(hdr.session_label, 0, sizeof(hdr.session_label));
    std::strncpy(hdr.session_label, m_session_label.c_str(),
                 sizeof(hdr.session_label) - 1);
    m_file.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    m_file.flush();
    m_file.close();
    m_recording.store(false);
}

void SpuEventCapture::write_event(uint8_t kind, const void* payload, uint16_t length) {
    if (!m_recording.load()) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_file.is_open() || !m_initial_state_written) return;

    EventStream::EventRecord rec{};
    rec.cycle = current_cycle();
    rec.kind = kind;
    rec.pad = 0;
    rec.length = length;
    rec.reserved = 0;
    m_file.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
    if (length > 0 && payload != nullptr) {
        m_file.write(reinterpret_cast<const char*>(payload), length);
    }
    m_event_count++;
}

void SpuEventCapture::emit_reg_write(uint16_t offset, uint16_t value) {
    uint16_t p[2] = { offset, value };
    write_event(EventStream::REG_WRITE, p, sizeof(p));
}

void SpuEventCapture::emit_reg_read(uint16_t offset, uint16_t returned) {
    uint16_t p[2] = { offset, returned };
    write_event(EventStream::REG_READ, p, sizeof(p));
}

void SpuEventCapture::emit_dma_in(const uint8_t* data, uint32_t bytes) {
    if (!m_recording.load()) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_file.is_open() || !m_initial_state_written) return;

    // Payload = u32 byte_count followed by `bytes` raw bytes.
    uint16_t payload_len = static_cast<uint16_t>(sizeof(uint32_t) + bytes);
    EventStream::EventRecord rec{};
    rec.cycle = current_cycle();
    rec.kind = EventStream::DMA_IN;
    rec.length = payload_len;
    m_file.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
    m_file.write(reinterpret_cast<const char*>(&bytes), sizeof(bytes));
    if (bytes > 0 && data != nullptr) {
        m_file.write(reinterpret_cast<const char*>(data), bytes);
    }
    m_event_count++;
}

void SpuEventCapture::emit_dma_out(const uint8_t* data, uint32_t bytes) {
    if (!m_recording.load()) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_file.is_open() || !m_initial_state_written) return;

    uint16_t payload_len = static_cast<uint16_t>(sizeof(uint32_t) + bytes);
    EventStream::EventRecord rec{};
    rec.cycle = current_cycle();
    rec.kind = EventStream::DMA_OUT;
    rec.length = payload_len;
    m_file.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
    m_file.write(reinterpret_cast<const char*>(&bytes), sizeof(bytes));
    if (bytes > 0 && data != nullptr) {
        m_file.write(reinterpret_cast<const char*>(data), bytes);
    }
    m_event_count++;
}

void SpuEventCapture::emit_xa(const uint8_t* data, uint32_t bytes) {
    if (!m_recording.load()) return;
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_file.is_open() || !m_initial_state_written) return;

    uint16_t payload_len = static_cast<uint16_t>(sizeof(uint32_t) + bytes);
    EventStream::EventRecord rec{};
    rec.cycle = current_cycle();
    rec.kind = EventStream::XA_IN;
    rec.length = payload_len;
    m_file.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
    m_file.write(reinterpret_cast<const char*>(&bytes), sizeof(bytes));
    if (bytes > 0 && data != nullptr) {
        m_file.write(reinterpret_cast<const char*>(data), bytes);
    }
    m_event_count++;
}

void SpuEventCapture::emit_sample_mark(uint64_t sample_idx) {
    write_event(EventStream::SAMPLE_MARK, &sample_idx, sizeof(sample_idx));
}

}  // namespace SPU
}  // namespace PCSX
