#ifndef AIRESULTREADER_H
#define AIRESULTREADER_H

#include <QThread>
#include <QString>
#include <cstdint>

/* AI verdict delivered to the Qt main thread whenever the motor_ai_client
 * publishes a new result snapshot to /motor_ai_result. Mirrors
 * ai_result_shm.h (ai_result_region_t / ai_result_snapshot_t). */
struct AiResult {
    uint64_t timestamp;
    uint32_t producer_seq;
    QString  anomaly;      /* "normal" or e.g. "anomaly"                    */
    QString  faultClass;   /* "none", "electrical", "mechanical", ...       */
    QString  rul;          /* remaining useful life string e.g. "RUL: 120"  */
};
Q_DECLARE_METATYPE(AiResult)

/* Background thread that polls the AI result shared-memory region
 * (AI_RESULT_SHM_NAME "/motor_ai_result") produced by motor_ai_client.
 * Emits newResult() only when the snapshot's seqlock/seq changes. */
class AiResultReader : public QThread {
    Q_OBJECT
public:
    explicit AiResultReader(QObject *parent = nullptr);
    void stop();

signals:
    void newResult(AiResult data);

protected:
    void run() override;

private:
    volatile bool m_running = true;
    void *m_shm             = nullptr;
    size_t m_shm_size       = 0;
};

#endif // AIRESULTREADER_H
