// High-level emulation of the GameCube AX audio microcode (HashEctor 0xe2136399).
// Adapted from Dolphin's AX HLE (Source/Core/Core/HW/DSPHLE/UCodes/AX.cpp, AXVoice.h,
// AXStructs.h and Core/DSP/DSPAccelerator.cpp), GPL-2.0-or-later, for a standalone host:
// guest main RAM and ARAM are plain buffers and replies go through a mail callback.
#include "ax_hle.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

namespace {

using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

#define HILO_TO_32(name) ((u32(name##_hi) << 16) | name##_lo)

// ---- guest memory (big-endian) ----
u8* g_ram;
u32 g_ram_size;
u8* g_aram;
u32 g_aram_size;
ax_mail_fn g_post_mail;
u64 g_cmdlists, g_voices_processed;

u8* ram_ptr(u32 addr, u32 len) {
    u32 off = addr & 0x01FFFFFFu;
    return off + len <= g_ram_size ? g_ram + off : nullptr;
}

u32 swap32(u32 v) { return __builtin_bswap32(v); }

void copy_from_emu_swapped16(void* dst, u32 addr, size_t bytes) {
    const u8* src = ram_ptr(addr, (u32)bytes);
    u16* d = static_cast<u16*>(dst);
    if (!src) { memset(dst, 0, bytes); return; }
    for (size_t i = 0; i < bytes / 2; ++i) d[i] = (u16)(src[2 * i] << 8 | src[2 * i + 1]);
}

void copy_to_emu_swapped16(u32 addr, const void* src, size_t bytes) {
    u8* dst = ram_ptr(addr, (u32)bytes);
    const u16* s = static_cast<const u16*>(src);
    if (!dst) return;
    for (size_t i = 0; i < bytes / 2; ++i) { dst[2 * i] = (u8)(s[i] >> 8); dst[2 * i + 1] = (u8)s[i]; }
}

void copy_to_emu_swapped32(u32 addr, const int* src, size_t bytes) {
    u8* dst = ram_ptr(addr, (u32)bytes);
    if (!dst) return;
    for (size_t i = 0; i < bytes / 4; ++i) {
        u32 v = swap32((u32)src[i]);
        memcpy(dst + 4 * i, &v, 4);
    }
}

int read_emu_s32(const u8* p, size_t i) {
    u32 v;
    memcpy(&v, p + 4 * i, 4);
    return (int)swap32(v);
}

s16 clamp_s16(s64 sample) { return (s16)std::clamp<s64>(sample, -0x8000, 0x7FFF); }

// ---- AXStructs.h (GameCube layout) ----
struct VolumeData { u16 volume; u16 volume_delta; };
struct PBMixer {
    VolumeData main_left, main_right, auxA_left, auxA_right, auxB_left, auxB_right;
    VolumeData auxB_surround, main_surround, auxA_surround;
};
struct PBInitialTimeDelay { u16 on, addrMemHigh, addrMemLow, offsetLeft, offsetRight, targetLeft, targetRight; };
struct PBUpdate { u16 pb_offset; u16 new_value; };
using PBUpdateData = std::array<PBUpdate, 32>;
struct PBUpdates { u16 num_updates[5]; u16 data_hi; u16 data_lo; };
struct PBDpop {
    s16 main_left, auxA_left, auxB_left, main_right, auxA_right, auxB_right, main_surround, auxA_surround,
        auxB_surround;
};
struct PBVolumeEnvelope { s16 cur_volume; s16 cur_volume_delta; };
struct PBUnknown2 { u16 unknown_reserved[3]; };
struct PBAudioAddr {
    u16 looping, sample_format, loop_addr_hi, loop_addr_lo, end_addr_hi, end_addr_lo, cur_addr_hi, cur_addr_lo;
};
struct PBADPCMInfo { s16 coefs[16]; u16 gain; u16 pred_scale; s16 yn1; s16 yn2; };
struct PBSampleRateConverter { u16 ratio_hi, ratio_lo, cur_addr_frac; s16 last_samples[4]; };
struct PBADPCMLoopInfo { u16 pred_scale; u16 yn1; u16 yn2; };
struct PBLowPassFilter { u16 on; s16 yn1; u16 a0; s16 b0; };

struct AXPB {
    u16 next_pb_hi, next_pb_lo, this_pb_hi, this_pb_lo;
    u16 src_type, coef_select, mixer_control, running, is_stream;
    PBMixer mixer;
    PBInitialTimeDelay initial_time_delay;
    PBUpdates updates;
    PBDpop dpop;
    PBVolumeEnvelope vol_env;
    PBUnknown2 unknown3;
    PBAudioAddr audio_addr;
    PBADPCMInfo adpcm;
    PBSampleRateConverter src;
    PBADPCMLoopInfo adpcm_loop_info;
    PBLowPassFilter lpf;
    u16 loop_counter;
    u16 padding[24];
};

enum { SRCTYPE_POLYPHASE = 0, SRCTYPE_LINEAR = 1, SRCTYPE_NEAREST = 2 };

enum AXMixControl : u32 {
    MIX_MAIN_L = 0x000001, MIX_MAIN_L_RAMP = 0x000002, MIX_MAIN_R = 0x000004, MIX_MAIN_R_RAMP = 0x000008,
    MIX_MAIN_S = 0x000010, MIX_MAIN_S_RAMP = 0x000020, MIX_AUXA_L = 0x000040, MIX_AUXA_L_RAMP = 0x000080,
    MIX_AUXA_R = 0x000100, MIX_AUXA_R_RAMP = 0x000200, MIX_AUXA_S = 0x000400, MIX_AUXA_S_RAMP = 0x000800,
    MIX_AUXB_L = 0x001000, MIX_AUXB_L_RAMP = 0x002000, MIX_AUXB_R = 0x004000, MIX_AUXB_R_RAMP = 0x008000,
    MIX_AUXB_S = 0x010000, MIX_AUXB_S_RAMP = 0x020000,
};

// Non-0x4e8a8b21 AX versions (ours) use this mixer_control layout.
u32 convert_mixer_control(u32 mc) {
    u32 ret = 0;
    if (mc & 0x0001) ret |= MIX_MAIN_L;
    if (mc & 0x0002) ret |= MIX_MAIN_R;
    if (mc & 0x0004) ret |= MIX_MAIN_S;
    if (mc & 0x0008) ret |= MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP | MIX_MAIN_S_RAMP;
    if (mc & 0x0010) ret |= MIX_AUXA_L;
    if (mc & 0x0020) ret |= MIX_AUXA_R;
    if (mc & 0x0040) ret |= MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
    if (mc & 0x0080) ret |= MIX_AUXA_S;
    if (mc & 0x0100) ret |= MIX_AUXA_S_RAMP;
    if (mc & 0x0200) ret |= MIX_AUXB_L;
    if (mc & 0x0400) ret |= MIX_AUXB_R;
    if (mc & 0x0800) ret |= MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
    if (mc & 0x1000) ret |= MIX_AUXB_S;
    if (mc & 0x2000) ret |= MIX_AUXB_S_RAMP;
    return ret;
}

// ---- DSPAccelerator.cpp ----
class Accelerator {
public:
    AXPB* pb = nullptr;

    void Setup(AXPB* p) {
        pb = p;
        start = HILO_TO_32(p->audio_addr.loop_addr) & 0x3fffffff;
        end = HILO_TO_32(p->audio_addr.end_addr) & 0x3fffffff;
        cur = HILO_TO_32(p->audio_addr.cur_addr) & 0xbfffffff;
        format = p->audio_addr.sample_format;
        yn1 = p->adpcm.yn1;
        yn2 = p->adpcm.yn2;
        reads_stopped = false;
        gain = (s16)p->adpcm.gain;
        pred_scale = p->adpcm.pred_scale & 0x7f;
    }

    u32 cur = 0;
    s16 yn1 = 0, yn2 = 0;
    u16 pred_scale = 0;

    u16 ReadSample(const s16* coefs) {
        if (reads_stopped) return 0;
        const u32 size = format & 3, decode = (format >> 2) & 3, gain_scale = (format >> 4) & 3;
        u16 val = 0;
        u8 step_size = 0;
        s16 raw = (s16)CurrentSample(size);
        const int coef_idx = (pred_scale >> 4) & 7;
        const s32 coef1 = coefs[coef_idx * 2 + 0], coef2 = coefs[coef_idx * 2 + 1];
        if (decode == 0) {   // ADPCM
            raw &= 0xF;
            const int scale = 1 << (pred_scale & 0xF);
            if (raw >= 8) raw -= 16;
            const s32 v32 = scale * raw + ((0x400 + coef1 * yn1 + coef2 * yn2) >> 11);
            val = (u16)clamp_s16(v32);
            step_size = 2;
            yn2 = yn1;
            yn1 = (s16)val;
            cur += 1;
            if ((end & 0xf) == 0x0 && cur == end) {
                cur = start + 1;
            } else if ((end & 0xf) == 0x1 && cur == end - 1) {
                cur = start;
            } else if ((cur & 15) == 0) {
                pred_scale = ReadMemory((cur & ~15u) >> 1);
                cur += 2;
                step_size += 2;
            }
        } else {             // PCM (ARAM or MMIO input; MMIO isn't used by AX)
            u8 shift = gain_scale == 1 ? 0 : gain_scale == 2 ? 16 : 11;
            const s32 v32 = ((s32(gain) * raw) >> shift) + (((coef1 * yn1) >> shift) + ((coef2 * yn2) >> shift));
            val = (u16)(s16)v32;
            yn2 = yn1;
            yn1 = (s16)val;
            step_size = 2;
            if (decode != 1) cur += 1;
        }
        if (cur == end + step_size - 1) {
            cur = start;
            reads_stopped = true;
            OnSampleReadEnd();
        }
        cur &= 0xbfffffff;
        return val;
    }

private:
    u32 start = 0, end = 0;
    u16 format = 0;
    s16 gain = 0;
    bool reads_stopped = false;

    u8 ReadMemory(u32 address) { return g_aram && address < g_aram_size ? g_aram[address] : 0; }

    u16 CurrentSample(u32 size) {
        switch (size) {
        case 0: {   // 4-bit
            u16 v = ReadMemory(cur >> 1);
            return (cur & 1) ? (v & 0xf) : (v >> 4);
        }
        case 1:     // 8-bit
            return ReadMemory(cur);
        case 2:     // 16-bit
            return (u16)(ReadMemory(cur * 2) << 8 | ReadMemory(cur * 2 + 1));
        default:
            return 0;
        }
    }

    // HLEAccelerator::OnSampleReadEndException
    void OnSampleReadEnd() {
        if (pb->audio_addr.looping) {
            pred_scale = pb->adpcm_loop_info.pred_scale & 0x7f;
            if (pb->is_stream != 1) {
                yn1 = (s16)pb->adpcm_loop_info.yn1;
                yn2 = (s16)pb->adpcm_loop_info.yn2;
            } else {
                pb->loop_counter++;
            }
            reads_stopped = false;
        } else {
            pb->running = 0;
        }
    }
};

// ---- AXVoice.h ----
struct AXBuffers {
    int* ptrs[9];   // main L/R/S, auxA L/R/S, auxB L/R/S
};

template <typename Input>
u32 resample(Input input, s16* output, u32 count, s16* last, u32 pos, u32 ratio, int srctype) {
    // Polyphase coefficients live in DSP ROM (not shipped): fall back to linear, like Dolphin.
    if (srctype == SRCTYPE_LINEAR || srctype == SRCTYPE_POLYPHASE) {
        s16 temp[4];
        u32 idx = 0;
        for (int i = 0; i < 4; ++i) temp[idx++ & 3] = last[i];
        for (u32 i = 0; i < count; ++i) {
            pos += ratio;
            while (pos >= 0x10000) {
                temp[idx++ & 3] = input();
                pos -= 0x10000;
            }
            const u16 frac = pos & 0xFFFF, inv = (u16)-frac;
            if (frac) {
                const s32 s0 = temp[idx++ & 3], s1 = temp[idx++ & 3];
                output[i] = (s16)(((s0 * inv) + (s1 * frac)) >> 16);
                idx += 2;
            } else {
                output[i] = temp[idx++ & 3];
                idx += 3;
            }
        }
        last[3] = temp[--idx & 3];
        last[2] = temp[--idx & 3];
        last[1] = temp[--idx & 3];
        last[0] = temp[--idx & 3];
    } else {
        for (u32 i = 0; i < count; ++i) output[i] = input();
        memcpy(last, output + count - 4, 4 * sizeof(s16));
    }
    return pos;
}

void mix_add(int* out, const s16* in, u32 count, VolumeData* vd, s16* dpop, bool ramp) {
    u16& volume = vd->volume;
    const u16 delta = ramp ? vd->volume_delta : 0;
    for (u32 i = 0; i < count; ++i) {
        const s16 s = clamp_s16(((s64)in[i] * volume) >> 15);
        out[i] += s;
        volume += delta;
        *dpop = s;
    }
}

void process_voice(Accelerator& acc, AXPB& pb, const AXBuffers& b, u16 count, u32 mctrl) {
    if (pb.running != 1) return;
    s16 samples[32];
    acc.Setup(&pb);
    const u32 pos = resample([&] { return (s16)acc.ReadSample(pb.adpcm.coefs); }, samples, count, pb.src.last_samples,
                             pb.src.cur_addr_frac, HILO_TO_32(pb.src.ratio), pb.src_type);
    pb.src.cur_addr_frac = pos & 0xFFFF;
    pb.audio_addr.cur_addr_hi = (u16)(acc.cur >> 16);
    pb.audio_addr.cur_addr_lo = (u16)acc.cur;
    pb.adpcm.yn1 = acc.yn1;
    pb.adpcm.yn2 = acc.yn2;
    pb.adpcm.pred_scale = acc.pred_scale;

    for (u32 i = 0; i < count; ++i) {
        samples[i] = clamp_s16(((s32)samples[i] * (s32)pb.vol_env.cur_volume) >> 15);
        pb.vol_env.cur_volume += pb.vol_env.cur_volume_delta;
    }
    if (pb.lpf.on)
        for (u32 i = 0; i < count; ++i)
            pb.lpf.yn1 = samples[i] = clamp_s16((pb.lpf.a0 * (s32)samples[i] + pb.lpf.b0 * (s32)pb.lpf.yn1) >> 15);

    VolumeData* vols[9] = {&pb.mixer.main_left, &pb.mixer.main_right, &pb.mixer.main_surround,
                           &pb.mixer.auxA_left, &pb.mixer.auxA_right, &pb.mixer.auxA_surround,
                           &pb.mixer.auxB_left, &pb.mixer.auxB_right, &pb.mixer.auxB_surround};
    s16* dpops[9] = {&pb.dpop.main_left, &pb.dpop.main_right, &pb.dpop.main_surround,
                     &pb.dpop.auxA_left, &pb.dpop.auxA_right, &pb.dpop.auxA_surround,
                     &pb.dpop.auxB_left, &pb.dpop.auxB_right, &pb.dpop.auxB_surround};
    static const u32 on_bits[9] = {MIX_MAIN_L, MIX_MAIN_R, MIX_MAIN_S, MIX_AUXA_L, MIX_AUXA_R,
                                   MIX_AUXA_S, MIX_AUXB_L, MIX_AUXB_R, MIX_AUXB_S};
    for (int c = 0; c < 9; ++c)
        if (mctrl & on_bits[c]) mix_add(b.ptrs[c], samples, count, vols[c], dpops[c], (mctrl & (on_bits[c] << 1)) != 0);
    g_voices_processed++;
}

// ---- AX.cpp ----
constexpr u32 SAMPLES = 5 * 32;   // 5 ms at 32 kHz
int s_buf[9][SAMPLES];             // main L/R/S, auxA L/R/S, auxB L/R/S
int (&s_main_l)[SAMPLES] = s_buf[0];
int (&s_main_r)[SAMPLES] = s_buf[1];
int (&s_main_s)[SAMPLES] = s_buf[2];
u16 s_cmdlist[512];
u32 s_cmdlist_size;
u16 s_compressor_pos;
Accelerator s_acc;

enum class MailState { WaitingForCmdListSize, WaitingForCmdListAddress, WaitingForNextTask };
MailState s_mail_state = MailState::WaitingForCmdListSize;

void setup_processing(u32 init_addr) {
    u16 init[27];
    copy_from_emu_swapped16(init, init_addr, sizeof(init));
    for (int i = 0; i < 9; ++i) {
        const s32 value = (s32)((u32(init[3 * i]) << 16) | init[3 * i + 1]);
        const s16 delta = (s16)init[3 * i + 2];
        for (u32 j = 0; j < SAMPLES; ++j) s_buf[i][j] = value ? value + (s32)j * delta : 0;
    }
}

void download_and_mix_with_volume(u32 addr, u16 vol_main, u16 vol_auxa, u16 vol_auxb) {
    const u16 vols[3] = {vol_main, vol_auxa, vol_auxb};
    for (u32 i = 0; i < 3; ++i) {
        const u8* p = ram_ptr(addr, 3 * SAMPLES * 4);
        if (!p) return;
        for (u32 j = 0; j < 3; ++j)
            for (u32 k = 0; k < SAMPLES; ++k)
                s_buf[i * 3 + j][k] += (s32)(((s64)read_emu_s32(p, j * SAMPLES + k) * vols[i]) >> 15);
    }
}

void process_pb_list(u32 pb_addr) {
    AXPB pb;
    // AX_LOG=1: every 1000 command lists (5 s), list the voices in the PB chain.
    static int log = getenv("AX_LOG") ? 1 : 0;
    const bool dump = log && g_cmdlists % 1000 == 0;
    if (dump) fprintf(stderr, "AX list %llu:\n", (unsigned long long)g_cmdlists);
    for (int guard = 0; pb_addr && guard < 512; ++guard) {
        AXBuffers b;
        for (int c = 0; c < 9; ++c) b.ptrs[c] = s_buf[c];
        copy_from_emu_swapped16(&pb, pb_addr, sizeof(pb));
        PBUpdateData updates;
        copy_from_emu_swapped16(updates.data(), HILO_TO_32(pb.updates.data), sizeof(updates));
        for (int ms = 0; ms < 5; ++ms) {
            u32 start = 0;
            for (int i = 0; i < ms; ++i) start += pb.updates.num_updates[i];
            const u16 n = pb.updates.num_updates[ms];
            if (start < updates.size() && n <= updates.size() - start)
                for (u32 i = start; i < start + n; ++i)
                    if (updates[i].pb_offset < sizeof(pb) / 2) reinterpret_cast<u16*>(&pb)[updates[i].pb_offset] = updates[i].new_value;
            process_voice(s_acc, pb, b, 32, convert_mixer_control(pb.mixer_control));
            for (auto& p : b.ptrs) p += 32;
        }
        copy_to_emu_swapped16(pb_addr, &pb, sizeof(pb));
        if (dump && pb.running)
            fprintf(stderr, "  pb %08X run=%u fmt=%u stream=%u loop=%u cur=%08X end=%08X ratio=%08X vol=%d mix=%04X L=%u R=%u\n",
                    pb_addr, pb.running, pb.audio_addr.sample_format, pb.is_stream, pb.audio_addr.looping,
                    HILO_TO_32(pb.audio_addr.cur_addr), HILO_TO_32(pb.audio_addr.end_addr), HILO_TO_32(pb.src.ratio),
                    pb.vol_env.cur_volume, pb.mixer_control, pb.mixer.main_left.volume, pb.mixer.main_right.volume);
        pb_addr = HILO_TO_32(pb.next_pb);
    }
}

void mix_aux_samples(int aux_id, u32 write_addr, u32 read_addr) {
    int* bufs = s_buf[3 + 3 * aux_id];
    if (write_addr)
        for (int i = 0; i < 3; ++i, write_addr += SAMPLES * 4) copy_to_emu_swapped32(write_addr, bufs + i * SAMPLES, SAMPLES * 4);
    const u8* p = ram_ptr(read_addr, 3 * SAMPLES * 4);
    if (!p) return;
    for (u32 c = 0; c < 3; ++c)
        for (u32 k = 0; k < SAMPLES; ++k) s_buf[c][k] += read_emu_s32(p, c * SAMPLES + k);
}

void upload_lrs(u32 dst) {
    for (int c = 0; c < 3; ++c, dst += SAMPLES * 4) copy_to_emu_swapped32(dst, s_buf[c], SAMPLES * 4);
}

void set_main_lr(u32 src) {
    const u8* p = ram_ptr(src, SAMPLES * 4);
    if (!p) return;
    for (u32 i = 0; i < SAMPLES; ++i) {
        const int s = read_emu_s32(p, i);
        s_main_l[i] = s;
        s_main_r[i] = s;
        s_main_s[i] = 0;
    }
}

void run_compressor(u16 threshold, u16 release_frames, u32 table_addr) {
    bool triggered = false;
    for (u32 i = 0; i < SAMPLES; ++i)
        if (std::abs(s_main_l[i]) > int(threshold) || std::abs(s_main_r[i]) > int(threshold)) { triggered = true; break; }
    const u32 frame_bytes = SAMPLES * 2;
    u32 table_offset;
    if (triggered) {
        table_offset = s_compressor_pos * frame_bytes;
        s_compressor_pos = release_frames;
    } else if (s_compressor_pos) {
        --s_compressor_pos;
        table_offset = (11 + s_compressor_pos) * frame_bytes;
    } else {
        return;
    }
    const u8* ramp = ram_ptr(table_addr + table_offset, SAMPLES * 2);
    if (!ramp) return;
    for (u32 i = 0; i < SAMPLES; ++i) {
        const u16 coef = (u16)(ramp[2 * i] << 8 | ramp[2 * i + 1]);
        s_main_l[i] = (int)(((s64)s_main_l[i] * coef) >> 15);
        s_main_r[i] = (int)(((s64)s_main_r[i] * coef) >> 15);
    }
}

void output_samples(u32 lr_addr, u32 surround_addr) {
    copy_to_emu_swapped32(surround_addr, s_main_s, SAMPLES * 4);
    u8* dst = ram_ptr(lr_addr, SAMPLES * 4);
    if (!dst) return;
    for (u32 i = 0; i < SAMPLES; ++i) {   // big-endian, right channel first
        const s16 l = clamp_s16(s_main_l[i]), r = clamp_s16(s_main_r[i]);
        dst[4 * i + 0] = (u8)((u16)r >> 8);
        dst[4 * i + 1] = (u8)r;
        dst[4 * i + 2] = (u8)((u16)l >> 8);
        dst[4 * i + 3] = (u8)l;
    }
}

void mix_auxb_lr(u32 ul_addr, u32 dl_addr) {
    copy_to_emu_swapped32(ul_addr, s_buf[6], SAMPLES * 4);
    copy_to_emu_swapped32(ul_addr + SAMPLES * 4, s_buf[7], SAMPLES * 4);
    const u8* p = ram_ptr(dl_addr, 2 * SAMPLES * 4);
    if (!p) return;
    for (u32 i = 0; i < SAMPLES; ++i) { const int s = read_emu_s32(p, i); s_buf[6][i] = s; s_main_l[i] += s; }
    for (u32 i = 0; i < SAMPLES; ++i) { const int s = read_emu_s32(p, SAMPLES + i); s_buf[7][i] = s; s_main_r[i] += s; }
}

void set_opposite_lr(u32 src) {
    const u8* p = ram_ptr(src, SAMPLES * 4);
    if (!p) return;
    for (u32 i = 0; i < SAMPLES; ++i) {
        const int s = read_emu_s32(p, i);
        s_main_l[i] = -s;
        s_main_r[i] = s;
        s_main_s[i] = 0;
    }
}

void send_aux_and_mix(u32 auxa_lrs_up, u32 auxb_s_up, u32 main_l_dl, u32 main_r_dl, u32 auxb_l_dl, u32 auxb_r_dl) {
    for (int i = 0; i < 3; ++i) copy_to_emu_swapped32(auxa_lrs_up + i * SAMPLES * 4, s_buf[3 + i], SAMPLES * 4);
    copy_to_emu_swapped32(auxb_s_up, s_buf[8], SAMPLES * 4);
    int* dl_bufs[4] = {s_main_l, s_main_r, s_buf[6], s_buf[7]};
    const u32 dl_addrs[4] = {main_l_dl, main_r_dl, auxb_l_dl, auxb_r_dl};
    for (int i = 0; i < 4; ++i) {
        const u8* p = ram_ptr(dl_addrs[i], SAMPLES * 4);
        if (!p) continue;
        for (u32 j = 0; j < SAMPLES; ++j) dl_bufs[i][j] += read_emu_s32(p, j);
    }
}

void handle_command_list() {
    u32 idx = 0;
    u32 pb_addr = 0;
    auto next = [&] { return idx < 512 ? s_cmdlist[idx++] : (u16)0x000F; };
    auto next32 = [&] { const u32 hi = next(); return hi << 16 | next(); };
    for (bool end = false; !end;) {
        const u16 cmd = next();
        switch (cmd) {
        case 0x00: setup_processing(next32()); break;                         // CMD_SETUP
        case 0x01: {                                                           // CMD_DL_AND_VOL_MIX
            const u32 a = next32();
            const u16 vm = next(), va = next(), vb = next();
            download_and_mix_with_volume(a, vm, va, vb);
            break;
        }
        case 0x02: pb_addr = next32(); break;                                 // CMD_PB_ADDR
        case 0x03: process_pb_list(pb_addr); break;                           // CMD_PROCESS
        case 0x04: case 0x05: {                                                // CMD_MIX_AUXA/B
            const u32 w = next32(), r = next32();
            mix_aux_samples(cmd - 0x04, w, r);
            break;
        }
        case 0x06: upload_lrs(next32()); break;                               // CMD_UPLOAD_LRS
        case 0x07: set_main_lr(next32()); break;                              // CMD_SET_LR
        case 0x08: idx += 10; break;                                          // CMD_UNK_08
        case 0x09: mix_aux_samples(1, 0, next32()); break;                    // CMD_MIX_AUXB_NOWRITE
        case 0x0A: case 0x0B: case 0x0C: break;
        case 0x0D: {                                                           // CMD_MORE
            const u32 a = next32();
            const u16 size = next();
            if (size < 512) copy_from_emu_swapped16(s_cmdlist, a, size * 2u);
            idx = 0;
            break;
        }
        case 0x0E: {                                                           // CMD_OUTPUT
            const u32 surround = next32(), lr = next32();
            output_samples(lr, surround);
            break;
        }
        case 0x0F: end = true; break;                                         // CMD_END
        case 0x10: { const u32 u = next32(), d = next32(); mix_auxb_lr(u, d); break; }
        case 0x11: set_opposite_lr(next32()); break;
        case 0x12: {                                                           // CMD_COMPRESSOR
            const u16 threshold = next(), frames = next();
            run_compressor(threshold, frames, next32());
            break;
        }
        case 0x13: {                                                           // CMD_SEND_AUX_AND_MIX
            u32 a[6];
            for (auto& x : a) x = next32();
            send_aux_and_mix(a[0], a[1], a[2], a[3], a[4], a[5]);
            break;
        }
        default: end = true; break;
        }
    }
}

}  // namespace

extern "C" {

void ax_init(u8* ram, u32 ram_size, u8* aram, u32 aram_size, ax_mail_fn post_mail) {
    g_ram = ram;
    g_ram_size = ram_size;
    g_aram = aram;
    g_aram_size = aram_size;
    g_post_mail = post_mail;
}

void ax_start(void) {
    s_mail_state = MailState::WaitingForCmdListSize;
    s_compressor_pos = 0;
    g_post_mail(0xDCD10000u, 1);   // DSP_INIT
}

void ax_resume(void) {
    s_mail_state = MailState::WaitingForCmdListSize;
    g_post_mail(0xDCD10001u, 1);   // DSP_RESUME
}

// Returns AX_RUNNING, AX_NEW_UCODE (another task follows; AX stays resident) or AX_RESET.
int ax_mail(u32 mail) {
    switch (s_mail_state) {
    case MailState::WaitingForCmdListSize:
        if ((mail & 0xFFFF0000u) == 0xBABE0000u) {
            s_cmdlist_size = mail & 0xFFFF;
            s_mail_state = MailState::WaitingForCmdListAddress;
        }
        return AX_RUNNING;
    case MailState::WaitingForCmdListAddress:
        if (s_cmdlist_size < 512) copy_from_emu_swapped16(s_cmdlist, mail, s_cmdlist_size * 2u);
        handle_command_list();
        g_cmdlists++;
        g_post_mail(0xDCD10002u, 1);   // DSP_YIELD
        s_mail_state = MailState::WaitingForNextTask;
        return AX_RUNNING;
    case MailState::WaitingForNextTask:
        mail = 0xCDD10000u | (mail & 0xFFFF);
        if (mail == 0xCDD10000u) {          // MAIL_RESUME
            g_post_mail(0xDCD10001u, 1);    // DSP_RESUME
            s_mail_state = MailState::WaitingForCmdListSize;
            return AX_RUNNING;
        }
        if (mail == 0xCDD10003u) {          // MAIL_CONTINUE
            s_mail_state = MailState::WaitingForCmdListSize;
            return AX_RUNNING;
        }
        if (mail == 0xCDD10001u) {          // MAIL_NEW_UCODE: ten task-description mails follow
            s_mail_state = MailState::WaitingForCmdListSize;
            return AX_NEW_UCODE;
        }
        return AX_RESET;                    // MAIL_RESET: back to the boot ROM
    }
    return AX_RUNNING;
}

// Command lists run to completion inside one mail, so only the mailbox protocol carries over.
void ax_state(StateIO* io) {
    STATE(io, s_mail_state);
    STATE(io, s_cmdlist_size);
    STATE(io, s_compressor_pos);
}

void ax_stats(unsigned long long* cmdlists, unsigned long long* voice_ms) {
    *cmdlists = g_cmdlists;
    *voice_ms = g_voices_processed;
}

}  // extern "C"
