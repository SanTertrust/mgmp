// mgmp_interstitials.cpp -- PassDay InterstitialQueue & House furniture layout sync.

#include "mgmp_house_internal.h"
#include "mgmp_bytestream.h"
#include "mgmp_leave.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_resolve.h"
#include "mgmp_savefile.h"

#include <cstring>
#include <functional>
#include <new>
#include <unordered_map>
#include <vector>

namespace mgmp {
namespace house_detail {
namespace {

void iq_w_u8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }
void iq_w_u32(std::vector<uint8_t>& out, uint32_t v) {
    const uint8_t* p = (const uint8_t*)&v;
    out.insert(out.end(), p, p + 4);
}
void iq_w_u64(std::vector<uint8_t>& out, uint64_t v) {
    const uint8_t* p = (const uint8_t*)&v;
    out.insert(out.end(), p, p + 8);
}
void iq_w_bytes(std::vector<uint8_t>& out, const void* data, uint32_t len) {
    iq_w_u32(out, len);
    if (len && data) {
        const uint8_t* p = (const uint8_t*)data;
        out.insert(out.end(), p, p + len);
    }
}
void iq_w_std_str(std::vector<uint8_t>& out, const void* std_str) {
    uint64_t len = 0, cap = 0;
    memcpy(&len, (const uint8_t*)std_str + 16, 8);
    memcpy(&cap, (const uint8_t*)std_str + 24, 8);
    const char* ptr = (cap > 15) ? *(const char* const*)std_str : (const char*)std_str;
    iq_w_bytes(out, ptr, (uint32_t)len);
}
void iq_w_std_wstr(std::vector<uint8_t>& out, const void* std_wstr) {
    uint64_t len = 0, cap = 0;
    memcpy(&len, (const uint8_t*)std_wstr + 16, 8);
    memcpy(&cap, (const uint8_t*)std_wstr + 24, 8);
    const wchar_t* ptr = (cap > 7) ? *(const wchar_t* const*)std_wstr : (const wchar_t*)std_wstr;
    iq_w_bytes(out, ptr, (uint32_t)(len * sizeof(wchar_t)));
}
bool iq_w_cat_vec(std::vector<uint8_t>& out, const void* vec) {
    if (!g.serialize_cat || !g.bs_dtor || !g.ofstream_ctor) return false;
    const uint8_t* first = nullptr;
    const uint8_t* last  = nullptr;
    memcpy(&first, (const uint8_t*)vec + 0, 8);
    memcpy(&last,  (const uint8_t*)vec + 8, 8);
    uint32_t count = (first && last >= first) ? (uint32_t)((last - first) / kCatData_Size) : 0;
    iq_w_u32(out, count);
    for (uint32_t i = 0; i < count; ++i) {
        void* cd = (void*)(first + (size_t)i * kCatData_Size);
        uint8_t bs[kByteStreamSize];
        memset(bs, 0, sizeof(bs));
        g.ofstream_ctor(bs + kBS_Ofstream);
        uint32_t mode = 1, swap = 8;
        memcpy(bs + kBS_Mode, &mode, 4);
        memcpy(bs + kBS_SwapElem, &swap, 4);
        g.serialize_cat(cd, bs, true);
        uint32_t wlen = 0;
        const void* wbuf = nullptr;
        memcpy(&wlen, bs + kBS_WriteLen, 4);
        memcpy(&wbuf, bs + kBS_WriteBuf, 8);
        iq_w_bytes(out, wbuf, wlen);
        g.bs_dtor(bs);
    }
    return true;
}

struct IqReader {
    const uint8_t* p = nullptr;
    uint32_t       len = 0;
    uint32_t       pos = 0;
    bool           ok  = true;

    uint8_t u8() {
        if (pos + 1 > len) { ok = false; return 0; }
        return p[pos++];
    }
    uint32_t u32() {
        if (pos + 4 > len) { ok = false; return 0; }
        uint32_t v = 0; memcpy(&v, p + pos, 4); pos += 4; return v;
    }
    uint64_t u64() {
        if (pos + 8 > len) { ok = false; return 0; }
        uint64_t v = 0; memcpy(&v, p + pos, 8); pos += 8; return v;
    }
    bool bytes(const uint8_t*& out_ptr, uint32_t& out_len) {
        out_len = u32();
        if (!ok || pos + out_len > len) { ok = false; return false; }
        out_ptr = p + pos;
        pos += out_len;
        return true;
    }
};

struct FurnitureRec {
    int64_t uid   = 0;
    int32_t x     = 0;
    int32_t y     = 0;
    int32_t fx    = 1;
    int32_t fy    = 1;
    int32_t depth = 0;
    uint8_t room_len = 0;
    char    room[64] = {};
};

void* current_house_inventory() {
    auto* slot = (void**)addr_of_data(D_HouseInventoryPtr);
    if (!slot) return nullptr;
    void* inv = nullptr;
    if (!mem_read(slot, &inv, sizeof(inv))) return nullptr;
    return inv;
}

bool serialize_furniture_layout(std::vector<uint8_t>& out) {
    out.clear();
    void* hinv = current_house_inventory();
    if (!hinv) return false;

    uint32_t count = 0;
    void** entries = nullptr;
    if (!mem_read((const uint8_t*)hinv + kHInv_FurnitureCount, &count, sizeof(count))) return false;
    if (count > 2048) return false;
    if (count > 0 && !mem_read((const uint8_t*)hinv + kHInv_FurnitureData, &entries, sizeof(entries)))
        return false;
    if (count > 0 && !entries) return false;

    auto push_bytes = [&](const void* p, size_t n) {
        const auto* b = (const uint8_t*)p;
        out.insert(out.end(), b, b + n);
    };

    uint16_t n16 = (uint16_t)count;
    push_bytes(&n16, sizeof(n16));
    uint16_t written = 0;
    for (uint32_t i = 0; i < count; ++i) {
        void* e = nullptr;
        if (!mem_read(entries + i, &e, sizeof(e)) || !e) continue;
        int64_t uid = 0;
        int32_t pos[2] = {};
        int32_t flip[2] = { 1, 1 };
        int32_t depth = 0;
        char room[64] = {};
        if (!mem_read((const uint8_t*)e + kFPE_Uid, &uid, sizeof(uid))) continue;
        mem_read((const uint8_t*)e + kFPE_PlacedPos, pos, sizeof(pos));
        mem_read((const uint8_t*)e + kFPE_PlacedFlip, flip, sizeof(flip));
        mem_read((const uint8_t*)e + kFPE_PlacedDepth, &depth, sizeof(depth));
        mem_read_std_string((const uint8_t*)e + kFPE_PlacedRoom, room, sizeof(room));
        uint8_t rlen = (uint8_t)strnlen(room, sizeof(room) - 1);

        push_bytes(&uid, sizeof(uid));
        push_bytes(pos, sizeof(pos));
        push_bytes(flip, sizeof(flip));
        push_bytes(&depth, sizeof(depth));
        push_bytes(&rlen, 1);
        if (rlen) push_bytes(room, rlen);
        ++written;
    }
    memcpy(out.data(), &written, sizeof(written));
    return true;
}

bool apply_furniture_layout(const uint8_t* data, uint32_t size, void* house_scene, void* house) {
    if (!data || size < 2 || !house_scene || !house) return false;
    if (!g.furn_remove || !g.create_furn_piece || !g.comp_destroy || !g.str_assign) return false;
    void* hinv = current_house_inventory();
    if (!hinv) return false;

    size_t off = 0;
    auto take = [&](void* dst, size_t n) -> bool {
        if (off + n > size) return false;
        memcpy(dst, data + off, n);
        off += n;
        return true;
    };

    uint16_t rec_count = 0;
    if (!take(&rec_count, sizeof(rec_count)) || rec_count > 2048) return false;

    std::vector<FurnitureRec> recs;
    recs.reserve(rec_count);
    for (uint16_t i = 0; i < rec_count; ++i) {
        FurnitureRec r{};
        if (!take(&r.uid, sizeof(r.uid))) return false;
        if (!take(&r.x, sizeof(r.x)) || !take(&r.y, sizeof(r.y))) return false;
        if (!take(&r.fx, sizeof(r.fx)) || !take(&r.fy, sizeof(r.fy))) return false;
        if (!take(&r.depth, sizeof(r.depth))) return false;
        if (!take(&r.room_len, 1)) return false;
        if (r.room_len >= sizeof(r.room)) return false;
        if (r.room_len && !take(r.room, r.room_len)) return false;
        r.room[r.room_len] = '\0';
        recs.push_back(r);
    }

    // Index local HouseInventory entries by uid so unknown uids are ignored.
    uint32_t local_count = 0;
    void** local_entries = nullptr;
    if (!mem_read((const uint8_t*)hinv + kHInv_FurnitureCount, &local_count, sizeof(local_count)) ||
        local_count > 2048)
        return false;
    if (local_count > 0 &&
        (!mem_read((const uint8_t*)hinv + kHInv_FurnitureData, &local_entries, sizeof(local_entries)) ||
         !local_entries))
        return false;

    std::unordered_map<int64_t, void*> by_uid;
    by_uid.reserve(local_count);
    for (uint32_t i = 0; i < local_count; ++i) {
        void* e = nullptr;
        int64_t uid = 0;
        if (mem_read(local_entries + i, &e, sizeof(e)) && e &&
            mem_read((const uint8_t*)e + kFPE_Uid, &uid, sizeof(uid)))
            by_uid[uid] = e;
    }

    auto* scene_current = (void**)addr_of_data(D_SceneCurrent);
    void* prev_scene = scene_current ? *scene_current : nullptr;
    if (scene_current) *scene_current = house_scene;

    // Remove and destroy currently placed FurniturePiece components.
    void** pieces = nullptr;
    uint32_t piece_count = 0;
    if (get_cached_list(house_scene, kCompId_FurniturePiece, &pieces, &piece_count) && pieces && piece_count > 0) {
        std::vector<void*> to_remove;
        to_remove.reserve(piece_count);
        for (uint32_t i = 0; i < piece_count; ++i) {
            void* p = pieces[i];
            if (!p) continue;
            void* entry = nullptr;
            void* grid  = nullptr;
            if (mem_read((const uint8_t*)p + kFPiece_Entry, &entry, sizeof(entry)) && entry &&
                mem_read((const uint8_t*)p + kFPiece_PlacedGrid, &grid, sizeof(grid)) && grid)
                to_remove.push_back(p);
        }
        for (void* p : to_remove) {
            g.furn_remove(p);
            g.comp_destroy(p);
        }
    }

    // Apply incoming layout to matching HouseInventory entries and re-spawn placed pieces.
    uint32_t applied = 0, spawned = 0;
    for (const auto& r : recs) {
        auto it = by_uid.find(r.uid);
        if (it == by_uid.end()) continue;
        uint8_t* entry = (uint8_t*)it->second;
        g.str_assign(entry + kFPE_PlacedRoom, r.room, r.room_len);
        int32_t pos[2]  = { r.x, r.y };
        int32_t flip[2] = { r.fx, r.fy };
        memcpy(entry + kFPE_PlacedPos,   pos,      sizeof(pos));
        memcpy(entry + kFPE_PlacedFlip,  flip,     sizeof(flip));
        memcpy(entry + kFPE_PlacedDepth, &r.depth, sizeof(r.depth));
        ++applied;
        if (r.room_len > 0) {
            int64_t uid_arg = r.uid;
            g.create_furn_piece(house, &uid_arg);
            ++spawned;
        }
    }

    if (scene_current) *scene_current = prev_scene;
    log_line("HOUSE", "applied furniture layout: %u/%u entries updated, %u placed",
             applied, (unsigned)recs.size(), spawned);
    return true;
}

} // namespace

bool serialize_interstitial_queue(const void* iq, std::vector<uint8_t>& out) {
    out.clear();
    if (!iq) return false;
    const uint8_t* q = (const uint8_t*)iq;
    iq_w_u8(out, q[kIQ_Flag0]);
    iq_w_u8(out, q[kIQ_Flag1]);
    uint32_t st = 0;
    memcpy(&st, q + kIQ_State, 4);
    iq_w_u32(out, st);

    auto write_str_vec = [&](const uint8_t* vec) {
        const uint8_t* first = nullptr;
        const uint8_t* last  = nullptr;
        memcpy(&first, vec + 0, 8);
        memcpy(&last,  vec + 8, 8);
        uint32_t n = (first && last >= first) ? (uint32_t)((last - first) / 32) : 0;
        iq_w_u32(out, n);
        for (uint32_t i = 0; i < n; ++i)
            iq_w_std_str(out, first + (size_t)i * 32);
    };
    write_str_vec(q + kIQ_NightCutscenes);
    write_str_vec(q + kIQ_DayCutscenes);

    const uint8_t* ifirst = nullptr;
    const uint8_t* ilast  = nullptr;
    memcpy(&ifirst, q + kIQ_Items + 0, 8);
    memcpy(&ilast,  q + kIQ_Items + 8, 8);
    uint32_t icount = (ifirst && ilast >= ifirst) ? (uint32_t)((ilast - ifirst) / kInterstitial_Size) : 0;
    iq_w_u32(out, icount);
    for (uint32_t i = 0; i < icount; ++i) {
        const uint8_t* it = ifirst + (size_t)i * kInterstitial_Size;
        iq_w_std_str(out,  it + kInterstitial_Str0);
        iq_w_std_str(out,  it + kInterstitial_Str1);
        iq_w_std_str(out,  it + kInterstitial_Str2);
        iq_w_std_wstr(out, it + kInterstitial_WStr0);
        iq_w_std_wstr(out, it + kInterstitial_WStr1);
        iq_w_u8(out, it[kInterstitial_Flag0]);
        iq_w_u8(out, it[kInterstitial_Flag1]);
        uint32_t pod_n = 0;
        const int64_t* pod_p = nullptr;
        memcpy(&pod_n, it + kInterstitial_Pod + 4, 4);
        memcpy(&pod_p, it + kInterstitial_Pod + 8, 8);
        iq_w_u32(out, pod_n);
        for (uint32_t j = 0; j < pod_n; ++j)
            iq_w_u64(out, (uint64_t)(pod_p ? pod_p[j] : 0));
        if (!iq_w_cat_vec(out, it + kInterstitial_Cats0)) return false;
        if (!iq_w_cat_vec(out, it + kInterstitial_Cats1)) return false;
        uint64_t tail = 0;
        memcpy(&tail, it + kInterstitial_TailU64, 8);
        iq_w_u64(out, tail);
    }
    return true;
}

bool deserialize_and_run_interstitials(const uint8_t* data, uint32_t size,
                                       const void* house_scene) {
    auto* scene_current = (void**)addr_of_data(D_SceneCurrent);
    if (!data || !size || !house_scene || !scene_current ||
        !g.do_interstitials || !g.inter_ctor || !g.inter_dtor ||
        !g.vec_inter_empl || !g.vec_str_push || !g.str_assign || !g.wstr_assign ||
        !g.podbuf_resize || !g.cat_ctor || !g.cat_dtor || !g.vec_cat_push ||
        !g.serialize_cat || !g.bs_dtor || !g.ofstream_ctor || !g.str_dtor) {
        log_line("HOUSE", "!! cannot start client interstitials (missing scene/resolver)");
        return false;
    }

    IqReader r{ data, size, 0, true };
    uint8_t iq[kInterstitialQueue_Size];
    memset(iq, 0, sizeof(iq));

    iq[kIQ_Flag0] = r.u8();
    iq[kIQ_Flag1] = r.u8();
    uint32_t st   = r.u32();
    memcpy(iq + kIQ_State, &st, 4);

    auto read_str_vec = [&](uint8_t* vec) -> bool {
        uint32_t n = r.u32();
        if (!r.ok || n > 4096) return false;
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* sp = nullptr; uint32_t sl = 0;
            if (!r.bytes(sp, sl)) return false;
            uint8_t gs[32];
            memset(gs, 0, sizeof(gs));
            uint64_t sso = 15;
            memcpy(gs + 24, &sso, 8);
            g.str_assign(gs, (const char*)sp, sl);
            g.vec_str_push(vec, gs);
            g.str_dtor(gs);
        }
        return true;
    };
    if (!read_str_vec(iq + kIQ_NightCutscenes)) return false;
    if (!read_str_vec(iq + kIQ_DayCutscenes))   return false;

    uint32_t icount = r.u32();
    if (!r.ok || icount > 512) return false;

    auto read_cat_vec = [&](uint8_t* vec) -> bool {
        uint32_t cn = r.u32();
        if (!r.ok || cn > 128) return false;
        for (uint32_t j = 0; j < cn; ++j) {
            const uint8_t* cp = nullptr; uint32_t cl = 0;
            if (!r.bytes(cp, cl)) return false;
            uint8_t cd[kCatData_Size];
            g.cat_ctor(cd);
            uint8_t bs[kByteStreamSize];
            memset(bs, 0, sizeof(bs));
            g.ofstream_ctor(bs + kBS_Ofstream);
            uint32_t mode = 0, swap = 8;
            memcpy(bs + kBS_Mode,     &mode, 4);
            memcpy(bs + kBS_ReadBuf,  &cp,   8);
            memcpy(bs + kBS_ReadLen,  &cl,   4);
            bs[kBS_ReadOwns] = 0;
            memcpy(bs + kBS_SwapElem, &swap, 4);
            g.serialize_cat(cd, bs, false);
            g.bs_dtor(bs);
            g.vec_cat_push(vec, cd);
            g.cat_dtor(cd);
        }
        return true;
    };

    for (uint32_t i = 0; i < icount; ++i) {
        uint8_t item[kInterstitial_Size];
        g.inter_ctor(item);
        const uint8_t* sp = nullptr; uint32_t sl = 0;
        if (!r.bytes(sp, sl)) { g.inter_dtor(item); return false; }
        g.str_assign(item + kInterstitial_Str0, (const char*)sp, sl);
        if (!r.bytes(sp, sl)) { g.inter_dtor(item); return false; }
        g.str_assign(item + kInterstitial_Str1, (const char*)sp, sl);
        if (!r.bytes(sp, sl)) { g.inter_dtor(item); return false; }
        g.str_assign(item + kInterstitial_Str2, (const char*)sp, sl);
        if (!r.bytes(sp, sl)) { g.inter_dtor(item); return false; }
        g.wstr_assign(item + kInterstitial_WStr0, (const wchar_t*)sp, sl / sizeof(wchar_t));
        if (!r.bytes(sp, sl)) { g.inter_dtor(item); return false; }
        g.wstr_assign(item + kInterstitial_WStr1, (const wchar_t*)sp, sl / sizeof(wchar_t));
        item[kInterstitial_Flag0] = r.u8();
        item[kInterstitial_Flag1] = r.u8();
        uint32_t pod_n = r.u32();
        if (!r.ok || pod_n > 1024) { g.inter_dtor(item); return false; }
        if (pod_n > 0) {
            g.podbuf_resize(item + kInterstitial_Pod, pod_n);
            memcpy(item + kInterstitial_Pod + 4, &pod_n, 4);
            int64_t* pod_p = nullptr;
            memcpy(&pod_p, item + kInterstitial_Pod + 8, 8);
            for (uint32_t j = 0; j < pod_n; ++j) {
                uint64_t v = r.u64();
                if (pod_p) pod_p[j] = (int64_t)v;
            }
        }
        if (!read_cat_vec(item + kInterstitial_Cats0) ||
            !read_cat_vec(item + kInterstitial_Cats1)) {
            g.inter_dtor(item);
            return false;
        }
        uint64_t tail = r.u64();
        memcpy(item + kInterstitial_TailU64, &tail, 8);
        if (!r.ok) { g.inter_dtor(item); return false; }

        void* last = nullptr;
        memcpy(&last, iq + kIQ_Items + 8, 8);
        g.vec_inter_empl(iq + kIQ_Items, last, item);
        g.inter_dtor(item);
    }

    // Unsuspend House scene once all interstitials finish.
    new (iq + kIQ_Callback) std::function<void()>([]() {
        const void* hs = nullptr;
        bool susp = false;
        if (leave_find_scene(kScene_House, &hs, &susp) && hs)
            *(uint8_t*)((uintptr_t)hs + kScene_Suspended) = 0;
    });

    log_line("HOUSE", "starting %u night/breeding interstitial(s) on client", icount);
    void* prev_scene = *scene_current;
    *scene_current = const_cast<void*>(house_scene);
    g.do_interstitials(iq);
    *scene_current = prev_scene;
    return true;
}

void interstitials_client_pump() {
    // Client: once the new House scene is loaded, kick off any queued
    // night/breeding interstitials for this generation before House ticks.
    if (g.has_queued_interstitial && g.queued_interstitial_gen == g.client_gen &&
        g.reload_stage == 0 && !savefile_has_pending()) {
        const void* hs = nullptr;
        bool hs_susp = false;
        if (leave_find_scene(kScene_House, &hs, &hs_susp) && hs) {
            *(uint8_t*)((uintptr_t)hs + kScene_Suspended) = 1;
            if (!deserialize_and_run_interstitials(g.queued_interstitial_blob.data(),
                                                   (uint32_t)g.queued_interstitial_blob.size(),
                                                   hs)) {
                *(uint8_t*)((uintptr_t)hs + kScene_Suspended) = 0;
            }
            g.has_queued_interstitial = false;
            g.queued_interstitial_blob.clear();
        }
    }
}

void interstitials_on_message(uint8_t from, const NetMsg& m) {
    switch (m.type) {
        case MSG_HOUSE_INTERSTITIALS: {
            if (!g.is_client || from != kHostPeer) break;
            const auto& im = m.house_interstitials;
            if (im.generation == g.client_gen && im.data && im.size > 0) {
                g.queued_interstitial_blob.assign(im.data, im.data + im.size);
                g.queued_interstitial_gen = im.generation;
                g.has_queued_interstitial = true;
                log_line("HOUSE", "<- INTERSTITIALS gen=%u bytes=%u",
                         im.generation, im.size);
            }
            break;
        }
        case MSG_HOUSE_POOP_POP: {
            const auto& pm = m.house_poop_pop;
            const uint32_t expected_gen = g.is_client ? g.client_gen : g.generation;
            if (pm.generation == 0 || pm.generation != expected_gen) break;
            if (g.is_client && from != kHostPeer) break;
            if (!g.poop_pop_orig) break;

            const void* hs = nullptr;
            bool hs_susp = false;
            if (leave_find_scene(kScene_House, &hs, &hs_susp) && hs) {
                void** pieces = nullptr;
                uint32_t count = 0;
                if (get_cached_list(const_cast<void*>(hs), kCompId_FurniturePiece, &pieces, &count) && pieces) {
                    for (uint32_t i = 0; i < count; ++i) {
                        void* piece = pieces[i];
                        if (!piece) continue;
                        void* entry = nullptr;
                        int64_t uid = 0;
                        if (!mem_read((const uint8_t*)piece + kFPiece_Entry, &entry, sizeof(entry)) || !entry)
                            continue;
                        if (!mem_read((const uint8_t*)piece + kFPiece_Uid, &uid, sizeof(uid)) || uid != pm.piece_uid)
                            continue;
                        auto* scene_current = (void**)addr_of_data(D_SceneCurrent);
                        void* prev_scene = scene_current ? *scene_current : nullptr;
                        if (scene_current) *scene_current = const_cast<void*>(hs);
                        g.applying_remote_poop_pop = true;
                        g.poop_pop_orig(piece);
                        g.applying_remote_poop_pop = false;
                        if (scene_current) *scene_current = prev_scene;
                        log_line("HOUSE", "<- POOP_POP uid=%lld from peer %u",
                                 (long long)pm.piece_uid, (unsigned)from);
                        break;
                    }
                }
            }
            if (!g.is_client) {
                // Relay to all clients (originating client already popped it locally, so its lookup is a no-op).
                net_send_house_poop_pop(pm);
            }
            break;
        }
        case MSG_HOUSE_FURNITURE: {
            const auto& fm = m.house_furniture;
            const uint32_t expected_gen = g.is_client ? g.client_gen : g.generation;
            if (fm.generation == 0 || fm.generation != expected_gen) break;
            if (g.is_client && from != kHostPeer) break;
            if (!fm.data || fm.size < 2) break;

            const void* hs = nullptr;
            bool hs_susp = false;
            if (leave_find_scene(kScene_House, &hs, &hs_susp) && hs) {
                void** hdata = nullptr;
                uint32_t hcount = 0;
                if (get_cached_list(const_cast<void*>(hs), kCompId_House, &hdata, &hcount) &&
                    hcount > 0 && hdata && hdata[0]) {
                    if (apply_furniture_layout(fm.data, fm.size, const_cast<void*>(hs), hdata[0])) {
                        log_line("HOUSE", "<- FURNITURE (%u bytes) from peer %u", fm.size, (unsigned)from);
                        if (!g.is_client) {
                            // Persist client's layout into host save without triggering a full client reload.
                            void* md = nullptr;
                            if (g.autosave_house && g.director_slot &&
                                mem_read(g.director_slot, &md, sizeof(md)) && md) {
                                g.suppress_autosave_publish = true;
                                g.autosave_house(md);
                                g.suppress_autosave_publish = false;
                            }
                            // Relay to any other connected clients (3-4 player sessions).
                            uint8_t ids[kMaxPeers]{};
                            if (net_peer_ids(ids, kMaxPeers)) {
                                for (uint8_t i = 0; i < net_peer_count(); ++i) {
                                    const uint8_t peer = ids[i];
                                    if (peer == kHostPeer || peer == from || peer >= kMaxPeers) continue;
                                    net_send_house_furniture_to(peer, fm);
                                }
                            }
                        }
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

} // namespace house_detail

void house_set_do_interstitials_orig(void* fn) {
    house_detail::g.do_interstitials = (house_detail::FnDoInterstitials)fn;
}

void house_on_do_interstitials(void* queue) {
    auto& g = house_detail::g;
    if (!g.on || g.is_client || !g.in_pass_day || !queue) return;
    if (house_detail::serialize_interstitial_queue(queue, g.pending_interstitial_blob)) {
        g.has_pending_interstitial_blob = true;
        log_line("HOUSE", "captured PassDay InterstitialQueue (%zu bytes)",
                 g.pending_interstitial_blob.size());
    } else {
        g.has_pending_interstitial_blob = false;
        g.pending_interstitial_blob.clear();
    }
}

void house_set_poop_pop_orig(void* fn) {
    house_detail::g.poop_pop_orig = (house_detail::FnVoidPtr)fn;
}

void house_on_poop_pop(void* piece) {
    auto& g = house_detail::g;
    if (!g.poop_pop_orig) return;
    if (!g.on || g.applying_remote_poop_pop || !piece) {
        g.poop_pop_orig(piece);
        return;
    }
    void* entry_before = nullptr;
    int64_t uid = 0;
    mem_read((const uint8_t*)piece + kFPiece_Entry, &entry_before, sizeof(entry_before));
    mem_read((const uint8_t*)piece + kFPiece_Uid, &uid, sizeof(uid));

    g.poop_pop_orig(piece);

    void* entry_after = nullptr;
    mem_read((const uint8_t*)piece + kFPiece_Entry, &entry_after, sizeof(entry_after));
    const uint32_t gen = g.is_client ? g.client_gen : g.generation;
    if (entry_before && !entry_after && gen > 0 && net_peer_count() > 0) {
        HousePoopPopMsg msg{};
        msg.generation = gen;
        msg.piece_uid  = uid;
        net_send_house_poop_pop(msg);
        log_line("HOUSE", "-> POOP_POP gen=%u uid=%lld", gen, (long long)uid);
    }
}

void house_on_layout_saved_pre() {
    auto& g = house_detail::g;
    if (g.on && !g.is_client && g.phase == HostPhase::House)
        g.suppress_autosave_publish = true;
}

void house_on_layout_saved() {
    auto& g = house_detail::g;
    g.suppress_autosave_publish = false;
    if (!g.on || net_peer_count() == 0) return;
    const uint32_t gen = g.is_client ? g.client_gen : g.generation;
    if (gen == 0) return;

    std::vector<uint8_t> blob;
    if (house_detail::serialize_furniture_layout(blob) && !blob.empty()) {
        HouseFurnitureMsg msg{};
        msg.generation = gen;
        msg.size       = (uint32_t)blob.size();
        msg.data       = blob.data();
        net_send_house_furniture(msg);
        log_line("HOUSE", "-> FURNITURE gen=%u (%u bytes)", gen, msg.size);
    } else if (!g.is_client && g.phase == HostPhase::House) {
        g.need_roster_publish = true;
    }
}

} // namespace mgmp
