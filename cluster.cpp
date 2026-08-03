#include "cluster.h"
#include <QDebug>
#include <QElapsedTimer>
#include <cmath>
#include <cstring>

#define SLOG_ERR(msg)  qWarning() << "[backend]" << msg
#define SLOG_INFO(msg) qDebug()   << "[backend]" << msg

template<typename T>
static inline T qnx_clamp(T val, T lo, T hi)
{
    return val < lo ? lo : (val > hi ? hi : val);
}

VehicleBackend::VehicleBackend(QObject *parent) : QObject(parent)
{
    m_powerWin = new float[POWER_WINDOW_SAMPLES];
    std::memset(m_powerWin, 0, sizeof(float) * POWER_WINDOW_SAMPLES);

    m_spiReader = new SpiReader(this);
    connect(m_spiReader, &SpiReader::newBlock,
            this,         &VehicleBackend::onBlockData,
            Qt::QueuedConnection);
    m_spiReader->start();
    SLOG_INFO("SpiReader thread started (motor_controller shm block consumer)");

    m_aiReader = new AiResultReader(this);
    connect(m_aiReader, &AiResultReader::newResult,
            this,        &VehicleBackend::onAiResult,
            Qt::QueuedConnection);
    m_aiReader->start();
    SLOG_INFO("AiResultReader thread started (motor_ai_client shm consumer)");
}

VehicleBackend::~VehicleBackend()
{
    if (m_spiReader) {
        m_spiReader->stop();
        m_spiReader->wait(1000);
    }
    if (m_aiReader) {
        m_aiReader->stop();
        m_aiReader->wait(1000);
    }
    delete[] m_powerWin;
}

/* Slot: called per producer block (~100 Hz) from the reader thread via
 * QueuedConnection. Runs on the Qt main thread, safe to touch QML state. */
void VehicleBackend::onBlockData(MotorBlock block)
{
    if (block.n_rows == 0) return;   /* defensive: producer never publishes empty blocks */

    /* --- Currents (8 ADC channels PA0..PA7) ---
     * Raw ADC is 0..4095. Convert to amps assuming a bipolar current-sense
     * amp with mid-rail zero (adjust ADC_MIDSCALE/AMPS_PER_COUNT in the
     * header for your hardware). */
    QVariantList amps;
    amps.reserve(8);
    float sum = 0.f, maxAbs = 0.f;
    bool changed = (m_currents.size() != 8);
    for (int i = 0; i < 8; ++i) {
        const float a = (static_cast<float>(block.rows[block.n_rows - 1].current[i]) - ADC_MIDSCALE) * AMPS_PER_COUNT;
        amps.append(a);
        sum += a;
        if (std::abs(a) > maxAbs) maxAbs = std::abs(a);
        if (!changed && m_currents[i].toFloat() != a) changed = true;
    }
    const float iMean = sum / 8.f;

    if (changed) {
        m_currents    = amps;
        m_currentMean = iMean;
        m_currentMax  = maxAbs;
        emit currentsChanged();
    }

    /* --- Vibration (three axes + magnitude) ---
     * MPU6050 at ±2g: 16384 counts/g. Total = vector magnitude in g. */
    const MotorRow &last = block.rows[block.n_rows - 1];
    const float vx = static_cast<float>(last.vib_x) / MPU_COUNTS_PER_G;
    const float vy = static_cast<float>(last.vib_y) / MPU_COUNTS_PER_G;
    const float vz = static_cast<float>(last.vib_z) / MPU_COUNTS_PER_G;
    const float vTotal = std::sqrt(vx*vx + vy*vy + vz*vz);

    if (vx != m_vibX || vy != m_vibY || vz != m_vibZ) {
        m_vibX = vx;
        m_vibY = vy;
        m_vibZ = vz;
        m_vibTotal = vTotal;
        emit vibChanged();
    }

    /* --- Power: slide every row of this block through the window.
     * Instantaneous three-phase power per row:
     *   p = Va·Ia + Vb·Ib + Vc·Ic            [W]
     * Window mean over the last POWER_WINDOW_SAMPLES rows:
     *   P_kW = windowSum / (1000 * windowCount)
     * The 20 kHz window (~50 ms) averages out mains/motor ripple.        */
    const float vScale = VOLTS_PER_COUNT;
    for (uint16_t i = 0; i < block.n_rows; ++i) {
        const MotorRow &r = block.rows[i];
        const float ia = (static_cast<float>(r.current[0]) - ADC_MIDSCALE) * AMPS_PER_COUNT;
        const float ib = (static_cast<float>(r.current[1]) - ADC_MIDSCALE) * AMPS_PER_COUNT;
        const float ic = (static_cast<float>(r.current[2]) - ADC_MIDSCALE) * AMPS_PER_COUNT;
        const float va = (static_cast<float>(r.current[3]) - ADC_MIDSCALE) * vScale;
        const float vb = (static_cast<float>(r.current[4]) - ADC_MIDSCALE) * vScale;
        const float vc = (static_cast<float>(r.current[5]) - ADC_MIDSCALE) * vScale;

        const float p = va*ia + vb*ib + vc*ic;

        if (m_powerCount == static_cast<size_t>(POWER_WINDOW_SAMPLES)) {
            m_powerSum -= m_powerWin[m_powerHead];
        } else {
            m_powerCount++;
        }
        m_powerWin[m_powerHead] = p;
        m_powerSum += p;
        m_powerHead = (m_powerHead + 1) % POWER_WINDOW_SAMPLES;
    }

    const float pKw = static_cast<float>(m_powerSum / (1000.0 * m_powerCount));

    /* --- Speed: km/h from motor rpm.
     *   v = (rpm * 2π * R * 3.6) / (60 * G)                                */
    const float r  = static_cast<float>(last.rpm);
    const float v  = r * SPEED_FACTOR;

    /* --- EWMA smoothing (kills noise; ~100 ms time constant) --- */
    if (m_rpm <= 0.f && m_speedKmh <= 0.f && m_powerKw <= 0.f) {
        /* first sample: seed the filters to avoid a slow ramp from zero */
        m_rpm      = r;
        m_speedKmh = v;
        m_powerKw  = pKw;
    } else {
        m_rpm      = EWMA_ALPHA * r + (1.f - EWMA_ALPHA) * m_rpm;
        m_speedKmh = EWMA_ALPHA * v + (1.f - EWMA_ALPHA) * m_speedKmh;
        m_powerKw  = EWMA_ALPHA * pKw + (1.f - EWMA_ALPHA) * m_powerKw;
    }

    evaluateWarnings();
    emitSmoothed();
}

/* Emit rpm/speed/power at most every EMIT_MS and only when the smoothed
 * value moved by more than the epsilon -- the human eye can't follow
 * 100 Hz updates, and a static needle shouldn't keep firing signals. */
void VehicleBackend::emitSmoothed()
{
    if (!m_emitTimer.isValid()) m_emitTimer.start();

    const qint64 now = m_emitTimer.elapsed();
    const bool timeUp = (now - m_lastEmitMs) >= EMIT_MS;
    if (!timeUp) return;

    m_lastEmitMs = now;

    const bool speedMoved = std::abs(m_speedKmh - m_lastSpeedEmit) >= SPEED_EPS_KMH;
    const bool powerMoved = std::abs(m_powerKw  - m_lastPowerEmit) >= POWER_EPS_KW;

    if (speedMoved) {
        m_lastSpeedEmit = m_speedKmh;
        emit rpmChanged();
        emit speedChanged();
    }
    if (powerMoved) {
        m_lastPowerEmit = m_powerKw;
        emit powerChanged();
    }
}

void VehicleBackend::onAiResult(AiResult result)
{
    m_aiStatus     = result.anomaly.isEmpty()   ? QStringLiteral("normal") : result.anomaly;
    m_aiFaultClass = result.faultClass.isEmpty() ? QStringLiteral("none")  : result.faultClass;
    m_aiRul        = result.rul.isEmpty()        ? QStringLiteral("RUL: N/A") : result.rul;

    const QString fc = m_aiFaultClass.toLower();
    m_aiNormal     = (m_aiStatus.toLower() == QLatin1String("normal")) || fc == QLatin1String("none");
    m_aiElectrical = fc.contains(QLatin1String("electrical"));
    m_aiMechanical = fc.contains(QLatin1String("mechanical"));

    SLOG_INFO("AI result: anomaly=" << m_aiStatus
              << " fault=" << m_aiFaultClass
              << " rul=" << m_aiRul);

    emit aiStatusChanged();
}

void VehicleBackend::evaluateWarnings()
{
    /* Vibration warning uses (total - 1g) so gravity doesn't trip it. */
    const float vibDynamic = std::abs(m_vibTotal - 1.f);

    const bool sw = m_rpm         >= SPEED_WARN;
    const bool vw = vibDynamic    >= VIB_WARN_G;
    const bool cw = m_currentMax  >= CURRENT_WARN_A;
    const bool crit = m_rpm       >= SPEED_CRIT
                   || vibDynamic  >= VIB_CRIT_G
                   || m_currentMax >= CURRENT_WARN_A * 1.5f;

    if (sw   != m_speedWarning)   { m_speedWarning   = sw;   emit speedWarningChanged();   }
    if (vw   != m_vibWarning)     { m_vibWarning     = vw;   emit vibWarningChanged();     }
    if (cw   != m_currentWarning) { m_currentWarning = cw;   emit currentWarningChanged(); }
    if (crit != m_criticalAlert)  { m_criticalAlert  = crit; emit criticalAlertChanged();  }
}
