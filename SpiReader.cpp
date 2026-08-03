#include "SpiReader.h"

/* --- Shared-memory contract -------------------------------------------
 *
 * The real layout lives in giga_spi/motor_shm.h (shm_region_t), but that
 * header uses C11 _Atomic / <stdatomic.h>. On this toolchain <stdatomic.h>'s
 * C++ compat shim only activates for C++23 (see
 * .../usr/include/c++/12.2.0/stdatomic.h: "#if __cplusplus > 202002L"), and
 * this project builds C++14 on QCC (CMakeLists.txt: "safer than 17 on QCC")
 * -- so motor_shm.h cannot be #included from a .cpp here.
 *
 * Instead, the structs below mirror shm_region_t's header + snapshot + block
 * ring byte-for-byte, using std::atomic in place of the producer's C11
 * _Atomic (same size/alignment for lock-free 4/8-byte types, so the layout
 * matches). We read both the snapshot (kept for backward compat) and the
 * block ring (whole blocks for windowed power computation).
 *
 * MOTOR_SHM_NAME, MOTOR_CACHELINE, MOTOR_RING_DEPTH and MOTOR_MAX_ROWS_PER_BLOCK
 * MUST stay in sync with giga_spi/motor_shm.h + motor_wire.h if those change.
 * -------------------------------------------------------------------- */
#define _Static_assert static_assert   /* motor_wire.h uses C11 _Static_assert */
#include "motor_wire.h"                 /* motor_row_t -- pure data layout, no atomics.
                                         * Vendored copy of the authoritative header from
                                         * the producer tree (giga_spi_8adc/motor_wire.h);
                                         * keep in sync -- ideally a git submodule.       */
#undef _Static_assert

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <climits>
#include <atomic>
#include <QDebug>

namespace {

constexpr const char *kMotorShmName  = "/motor_ctrl"; // motor_shm.h: MOTOR_SHM_NAME
constexpr size_t      kMotorCacheline = 64;            // motor_shm.h: MOTOR_CACHELINE
constexpr uint32_t    kMotorShmMagic  = 0x4D435452u;   // motor_shm.h: MOTOR_SHM_MAGIC ("MCTR")
constexpr uint32_t    kMotorRingDepth = 16u;           // motor_shm.h: MOTOR_RING_DEPTH
constexpr uint16_t    kMaxRows        = 200u;          // motor_wire.h: MOTOR_MAX_ROWS_PER_BLOCK

struct MotorSnapshotShm {
    alignas(kMotorCacheline) std::atomic<uint32_t> seqlock;
    uint32_t     producer_seq;
    uint64_t     timestamp;
    uint16_t     flags;
    uint16_t     _pad;
    motor_row_t  row;
};

struct RingSlotShm {
    alignas(kMotorCacheline) std::atomic<uint32_t> seq; /* odd while producer writes */
    uint32_t     producer_seq;
    uint64_t     timestamp;
    uint16_t     n_rows;
    uint16_t     flags;
    uint64_t     row_ts[kMaxRows];
    motor_row_t  rows[kMaxRows];
};

struct BlockRingShm {
    alignas(kMotorCacheline) std::atomic<uint64_t> write_pos;
    uint32_t     depth;
    uint32_t     _pad;
    RingSlotShm  block_slots[kMotorRingDepth];  /* 'slots' is a Qt macro -- don't use it */
};

struct ShmRegionHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t _pad0;
    uint32_t row_size;
    uint32_t reserved;
    alignas(kMotorCacheline) MotorSnapshotShm snapshot;
    alignas(kMotorCacheline) BlockRingShm ring;
};

/* Bytes up to and including the ring -- what we mmap. */
constexpr size_t kShmRegionBytes = sizeof(ShmRegionHeader);

/* True if our C++ mirrors are layout-compatible with the C producer structs.
 * The numbers below are verified against giga_spi/motor_shm.h by compiling it
 * with the real C compiler (see layout probe in the repo docs):
 *   shm_snapshot_t   : size 64,    row@20
 *   shm_block_t      : size 6464,  n_rows@16, row_ts@24, rows@1624
 *   shm_block_ring_t : size 103488, depth@8,  slots@64
 *   shm_region_t     : size 103616, snapshot@64, ring@128                    */
static_assert(offsetof(MotorSnapshotShm, row)          == 20,    "snapshot row offset mismatch");
static_assert(offsetof(RingSlotShm, n_rows)            == 16,    "ring slot n_rows offset mismatch");
static_assert(offsetof(RingSlotShm, row_ts)            == 24,    "ring slot row_ts offset mismatch");
static_assert(offsetof(RingSlotShm, rows)              == 1624,  "ring slot rows offset mismatch");
static_assert(offsetof(BlockRingShm, block_slots)      == 64,    "ring slots offset mismatch");
static_assert(offsetof(ShmRegionHeader, snapshot)      == 64,    "region snapshot offset mismatch");
static_assert(offsetof(ShmRegionHeader, ring)          == 128,   "region ring offset mismatch");
static_assert(sizeof(ShmRegionHeader)                  == 103616, "region size mismatch");

} // namespace

SpiReader::SpiReader(QObject *parent) : QThread(parent)
{
    qRegisterMetaType<MotorBlock>("MotorBlock");
}

/* Copy one block slot into our MotorBlock. Called between two seqlock reads
 * by the retry loop below. */
static void copy_block(const RingSlotShm *src, MotorBlock *dst)
{
    uint16_t n = src->n_rows;
    if (n > kMaxRows) n = kMaxRows;

    dst->seq       = src->producer_seq;
    dst->timestamp = src->timestamp;
    dst->n_rows    = n;
    dst->flags     = src->flags;

    for (uint16_t i = 0; i < n; ++i) {
        const motor_row_t &r = src->rows[i];
        MotorRow &out = dst->rows[i];
        for (int c = 0; c < 8; ++c)
            out.current[c] = r.current[c];
        out.vib_x = r.vib_x;
        out.vib_y = r.vib_y;
        out.vib_z = r.vib_z;
        out.rpm   = r.rpm;
    }
}

void SpiReader::run()
{
    /* Retry shm_open for up to 5s in case motor_controller is starting
     * slightly after us.                                                 */
    int fd = -1;
    for (int attempt = 0; attempt < 20 && m_running; ++attempt) {
        fd = shm_open(kMotorShmName, O_RDONLY, 0);
        if (fd != -1) break;
        usleep(250000);
    }
    if (fd == -1) {
        qWarning("SpiReader: shm_open(%s) failed -- is motor_controller running?",
                 kMotorShmName);
        return;
    }

    m_shm_size = kShmRegionBytes;
    m_shm = mmap(nullptr, m_shm_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m_shm == MAP_FAILED) {
        qWarning("SpiReader: mmap failed");
        m_shm = nullptr;
        return;
    }

    const ShmRegionHeader *region = static_cast<const ShmRegionHeader *>(m_shm);

    /* Contract guard: refuse to read a region written by a producer with a
     * different wire/shm layout, otherwise we'd emit garbage. Mirrors the
     * checks in motor_shm_region_valid().                                    */
    if (region->magic != kMotorShmMagic
        || region->version != static_cast<uint16_t>(MOTOR_CONTRACT_VERSION)
        || region->row_size != static_cast<uint32_t>(sizeof(motor_row_t))) {
        qWarning("SpiReader: shm contract mismatch "
                 "(magic=0x%08x ver=%u row=%u; expected 0x%08x ver=%u row=%zu) -- not reading",
                 region->magic, region->version, region->row_size,
                 kMotorShmMagic, (unsigned)MOTOR_CONTRACT_VERSION, sizeof(motor_row_t));
        munmap(m_shm, m_shm_size);
        m_shm = nullptr;
        return;
    }

    const BlockRingShm *ring = &region->ring;
    if (ring->depth != kMotorRingDepth) {
        qWarning("SpiReader: ring depth mismatch (%u != %u) -- not reading",
                 ring->depth, (unsigned)kMotorRingDepth);
        munmap(m_shm, m_shm_size);
        m_shm = nullptr;
        return;
    }

    uint64_t last_pos = 0;   /* one past the newest block we already consumed */

    while (m_running) {
        /* Producer commits a slot, then bumps write_pos with release
         * ordering; the acquire load below gives us that ordering.        */
        uint64_t pos = ring->write_pos.load(std::memory_order_acquire);

        while (last_pos < pos) {
            const RingSlotShm *slot = &ring->block_slots[last_pos % kMotorRingDepth];

            MotorBlock local;
            int tries = 0;
            bool ok = false;
            for (; tries < 64; ++tries) {
                uint32_t s1 = slot->seq.load(std::memory_order_acquire);
                if (s1 & 1u) continue;              /* producer writing   */
                copy_block(slot, &local);
                uint32_t s2 = slot->seq.load(std::memory_order_acquire);
                if (s1 == s2) { ok = true; break; } /* clean read          */
            }
            if (ok)
                emit newBlock(local);
            ++last_pos;
        }

        usleep(10000);  /* 10 ms poll; well above the 10 ms block cadence */
    }

    munmap(m_shm, m_shm_size);
    m_shm = nullptr;
}

void SpiReader::stop()
{
    m_running = false;
}
