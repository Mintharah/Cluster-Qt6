#include "AiResultReader.h"

/* --- Shared-memory contract -------------------------------------------
 * Mirrors motor_ai_client/client/src/ai_result_shm.h. The producer uses C11
 * _Atomic; we mirror with std::atomic (same size/alignment for lock-free
 * 4-byte type). Keep in sync with that header if it changes.
 * -------------------------------------------------------------------- */
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <atomic>
#include <QDebug>

namespace {

constexpr const char *kAiShmName  = "/motor_ai_result"; /* ai_result_shm.h */
constexpr uint32_t    kAiMagic    = 0x41495200u;        /* "AIR\0"         */
constexpr uint32_t    kAiVersion  = 1u;
constexpr size_t      kAiCacheline = 64u;
constexpr size_t      kAiStrLen    = 256u;

struct AiSnapshotShm {
    alignas(kAiCacheline) std::atomic<uint32_t> seqlock;
    uint64_t     timestamp;
    uint32_t     producer_seq;
    uint16_t     flags;
    uint16_t     _pad;
    char         anomaly_result[kAiStrLen];
    char         fault_class_result[kAiStrLen];
    char         pred_maint_result[kAiStrLen];
};

struct AiRegionShm {
    uint32_t magic;
    uint16_t version;
    uint16_t _pad0;
    alignas(kAiCacheline) AiSnapshotShm snapshot;
};

} // namespace

AiResultReader::AiResultReader(QObject *parent) : QThread(parent)
{
    qRegisterMetaType<AiResult>("AiResult");
}

void AiResultReader::run()
{
    int fd = -1;
    for (int attempt = 0; attempt < 20 && m_running; ++attempt) {
        fd = shm_open(kAiShmName, O_RDONLY, 0);
        if (fd != -1) break;
        usleep(250000);
    }
    if (fd == -1) {
        qWarning("AiResultReader: shm_open(%s) failed -- is motor_ai_client running?",
                 kAiShmName);
        return;
    }

    m_shm_size = sizeof(AiRegionShm);
    m_shm = mmap(nullptr, m_shm_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m_shm == MAP_FAILED) {
        qWarning("AiResultReader: mmap failed");
        m_shm = nullptr;
        return;
    }

    const AiRegionShm *region = static_cast<const AiRegionShm *>(m_shm);

    if (region->magic != kAiMagic || region->version != kAiVersion) {
        qWarning("AiResultReader: shm contract mismatch "
                 "(magic=0x%08x ver=%u; expected 0x%08x ver=%u) -- not reading",
                 region->magic, region->version, kAiMagic, (unsigned)kAiVersion);
        munmap(m_shm, m_shm_size);
        m_shm = nullptr;
        return;
    }

    const AiSnapshotShm *snap = &region->snapshot;

    uint32_t last_seq = UINT32_MAX;

    while (m_running) {
        AiResult local;
        bool have = false;

        int tries = 0;
        for (;; ++tries) {
            uint32_t s1 = snap->seqlock.load(std::memory_order_acquire);
            if (s1 & 1u) { /* writer in progress */ if (tries > 100) break; continue; }
            local.timestamp    = snap->timestamp;
            local.producer_seq = snap->producer_seq;
            local.anomaly      = QString::fromUtf8(snap->anomaly_result);
            local.faultClass   = QString::fromUtf8(snap->fault_class_result);
            local.rul          = QString::fromUtf8(snap->pred_maint_result);
            uint32_t s2 = snap->seqlock.load(std::memory_order_acquire);
            if (s1 == s2) { have = true; break; }
            if (tries > 100) break;
        }

        if (have && local.producer_seq != last_seq) {
            last_seq = local.producer_seq;
            emit newResult(local);
        }

        usleep(200000);  /* 200 ms poll; results arrive ~ every 2 s per window */
    }

    munmap(m_shm, m_shm_size);
    m_shm = nullptr;
}

void AiResultReader::stop()
{
    m_running = false;
}
