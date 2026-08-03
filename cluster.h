#ifndef CLUSTER_H
#define CLUSTER_H

#pragma once
#include <QObject>
#include <QString>
#include <QVariant>
#include <QElapsedTimer>
#include <qqml.h>
#include "SpiReader.h"     /* MotorBlock */
#include "AiResultReader.h" /* AiResult  */

/* VehicleBackend: presents the motor sensor snapshot + AI verdict as QML
 * properties.
 *
 * Sensor set (matches motor_wire.h v4):
 *   - 8 ADC channels (PA0..PA7, raw counts):
 *       [0..2] = phase currents A/B/C
 *       [3..5] = phase voltages A/B/C
 *       [6]    = DC-bus voltage
 *       [7]    = speed voltage
 *   - 3-axis vibration (MPU6050, ±2g, raw counts) -> vibX/Y/Z + vibTotal (g)
 *   - rpm (from tach input capture)
 *
 * Derived values:
 *   - speed (km/h) = (rpm * 2π * R * 3.6) / (60 * G)   [WHEEL_RADIUS_M/GEAR_RATIO]
 *   - power (kW)   = Σ(Va·Ia + Vb·Ib + Vc·Ic) / (1000 * N) over a sliding
 *     window of POWER_WINDOW_SAMPLES rows (default 1000 ≈ 50 ms @ 20 kHz).
 *   Both are EWMA-smoothed and emitted at most every EMIT_MS to match what a
 *   human eye can follow.
 *
 * AI status from /motor_ai_result (written by motor_ai_client):
 *   - aiStatus:     "normal" / "anomaly"
 *   - aiFaultClass: "none" / "electrical" / "mechanical" / ...
 *   - aiRul:        remaining-useful-life string
 *
 * Legacy properties still declared for QML compatibility (temp, battery,
 * and the temp/voltage warning flags) -- they stay at 0/false. */
class VehicleBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    /* Real sensor properties. */
    Q_PROPERTY(float rpm             READ rpm             NOTIFY rpmChanged)
    Q_PROPERTY(float speed           READ speed           NOTIFY speedChanged)   /* km/h */
    Q_PROPERTY(float speedRpm        READ speedRpm        NOTIFY rpmChanged)     /* raw rpm */

    Q_PROPERTY(QVariantList currents READ currents       NOTIFY currentsChanged) /* 8 scaled amps */
    Q_PROPERTY(float currentPhaseA   READ currentPhaseA   NOTIFY currentsChanged) /* == currents[0] */
    Q_PROPERTY(float currentPhaseB   READ currentPhaseB   NOTIFY currentsChanged) /* == currents[1] */
    Q_PROPERTY(float currentPhaseC   READ currentPhaseC   NOTIFY currentsChanged) /* == currents[2] */
    Q_PROPERTY(float currentMean     READ currentMean     NOTIFY currentsChanged)
    Q_PROPERTY(float currentMax      READ currentMax      NOTIFY currentsChanged) /* max |channel| */

    Q_PROPERTY(float vibX            READ vibX            NOTIFY vibChanged)
    Q_PROPERTY(float vibY            READ vibY            NOTIFY vibChanged)
    Q_PROPERTY(float vibZ            READ vibZ            NOTIFY vibChanged)
    Q_PROPERTY(float vibTotal        READ vibTotal        NOTIFY vibChanged)

    /* Derived power (kW), from the three phase currents + voltages. */
    Q_PROPERTY(float power           READ power           NOTIFY powerChanged)

    /* AI status (from motor_ai_client -> /motor_ai_result). */
    Q_PROPERTY(QString aiStatus      READ aiStatus        NOTIFY aiStatusChanged)
    Q_PROPERTY(QString aiFaultClass  READ aiFaultClass    NOTIFY aiStatusChanged)
    Q_PROPERTY(QString aiRul         READ aiRul           NOTIFY aiStatusChanged)
    Q_PROPERTY(bool aiNormal         READ aiNormal        NOTIFY aiStatusChanged)
    Q_PROPERTY(bool aiElectrical     READ aiElectrical    NOTIFY aiStatusChanged)
    Q_PROPERTY(bool aiMechanical     READ aiMechanical    NOTIFY aiStatusChanged)

    /* Legacy properties kept so existing QML doesn't need to change.
     * They read as 0 since we don't measure them.                       */
    Q_PROPERTY(float temp            READ temp            NOTIFY tempChanged)
    Q_PROPERTY(float voltage         READ voltage         NOTIFY voltageChanged)
    Q_PROPERTY(float battery         READ battery         NOTIFY batteryChanged)
    Q_PROPERTY(float current         READ current         NOTIFY currentsChanged) /* == currentMean */

    /* Legacy warning flags referenced by Main.qml; no v4 source, always false. */
    Q_PROPERTY(bool tempWarning      READ tempWarning     NOTIFY tempWarningChanged)
    Q_PROPERTY(bool voltageWarning   READ voltageWarning  NOTIFY voltageWarningChanged)

    /* Warnings. */
    Q_PROPERTY(bool speedWarning     READ speedWarning    NOTIFY speedWarningChanged)
    Q_PROPERTY(bool vibWarning       READ vibWarning      NOTIFY vibWarningChanged)
    Q_PROPERTY(bool currentWarning   READ currentWarning  NOTIFY currentWarningChanged)
    Q_PROPERTY(bool criticalAlert    READ criticalAlert   NOTIFY criticalAlertChanged)

public:
    explicit VehicleBackend(QObject *parent = nullptr);
    ~VehicleBackend();

    /* Thresholds -- tune to your motor / test rig. */
    static constexpr float SPEED_WARN     = 3000.f;    /* RPM                    */
    static constexpr float SPEED_CRIT     = 5000.f;
    static constexpr float VIB_WARN_G     = 2.f;       /* total g magnitude      */
    static constexpr float VIB_CRIT_G     = 4.f;
    static constexpr float CURRENT_WARN_A = 50.f;      /* amps, after scaling    */

    /* Raw MPU6050 sensitivity at ±2g: 16384 counts per g. */
    static constexpr float MPU_COUNTS_PER_G = 16384.f;

    /* Current sensor scaling -- adjust for your INA199 / shunt combo.
     * Assumes bipolar current sense with 3.3V ADC and mid-rail zero. */
    static constexpr float ADC_MIDSCALE    = 2048.f;
    static constexpr float AMPS_PER_COUNT  = 0.05f;    /* 100 A full-scale -> 0.05 A / count */

    /* Voltage sensor scaling -- tune VOLTS_PER_COUNT to your divider. */
    static constexpr float VOLTS_PER_COUNT = 0.01f;    /* volts / ADC count */

    /* Vehicle constants for speed conversion:
     *   v_kmh = (rpm * 2π * R * 3.6) / (60 * G)                              */
    static constexpr float WHEEL_RADIUS_M  = 0.33f;    /* tire radius (m)      */
    static constexpr float GEAR_RATIO      = 9.0f;     /* final drive ratio    */

    /* Power window: number of rows averaged into one power reading.
     * 1000 rows @ 20 kHz = 50 ms. */
    static constexpr int   POWER_WINDOW_SAMPLES = 1000;
    static constexpr float SPEED_FACTOR  = (2.f * 3.14159265f * WHEEL_RADIUS_M * 3.6f) / (60.f * GEAR_RATIO);

    /* Noise / human-eye smoothing. */
    static constexpr float EWMA_ALPHA     = 0.1f;      /* ~100 ms time constant @ 100 Hz */
    static constexpr int   EMIT_MS        = 100;       /* max QML update rate             */
    static constexpr float SPEED_EPS_KMH  = 0.5f;      /* emit only above this change     */
    static constexpr float POWER_EPS_KW   = 0.05f;

    float rpm()           const { return m_rpm; }
    float speed()         const { return m_speedKmh; }       /* smoothed km/h */
    float speedRpm()      const { return m_rpm; }            /* smoothed rpm  */

    QVariantList  currents()    const { return m_currents; }
    float currentPhaseA() const { return m_currents.size() > 0 ? m_currents[0].toFloat() : 0.f; }
    float currentPhaseB() const { return m_currents.size() > 1 ? m_currents[1].toFloat() : 0.f; }
    float currentPhaseC() const { return m_currents.size() > 2 ? m_currents[2].toFloat() : 0.f; }
    float currentMean()   const { return m_currentMean; }
    float currentMax()    const { return m_currentMax; }
    float current()       const { return m_currentMean; }

    float vibX()          const { return m_vibX; }
    float vibY()          const { return m_vibY; }
    float vibZ()          const { return m_vibZ; }
    float vibTotal()      const { return m_vibTotal; }

    float power()         const { return m_powerKw; }        /* smoothed kW */

    QString aiStatus()    const { return m_aiStatus; }
    QString aiFaultClass() const { return m_aiFaultClass; }
    QString aiRul()       const { return m_aiRul; }
    bool    aiNormal()    const { return m_aiNormal; }
    bool    aiElectrical() const { return m_aiElectrical; }
    bool    aiMechanical() const { return m_aiMechanical; }

    /* legacy no-signal fields */
    float temp()          const { return 0.f; }
    float voltage()       const { return 0.f; }
    float battery()       const { return 0.f; }
    bool  tempWarning()   const { return false; }
    bool  voltageWarning() const { return false; }

    bool speedWarning()   const { return m_speedWarning; }
    bool vibWarning()     const { return m_vibWarning; }
    bool currentWarning() const { return m_currentWarning; }
    bool criticalAlert()  const { return m_criticalAlert; }

public slots:
    void onBlockData(MotorBlock block);
    void onAiResult(AiResult result);

signals:
    void rpmChanged();
    void speedChanged();
    void currentsChanged();
    void vibChanged();
    void powerChanged();
    void tempChanged();
    void voltageChanged();
    void batteryChanged();
    void tempWarningChanged();
    void voltageWarningChanged();

    void speedWarningChanged();
    void vibWarningChanged();
    void currentWarningChanged();
    void criticalAlertChanged();

    void aiStatusChanged();

private:
    void evaluateWarnings();
    void emitSmoothed();

    SpiReader *m_spiReader = nullptr;
    AiResultReader *m_aiReader = nullptr;

    float m_rpm         = 0.f;         /* EWMA-smoothed rpm       */
    float m_speedKmh    = 0.f;         /* EWMA-smoothed km/h      */
    float m_powerKw     = 0.f;         /* EWMA-smoothed kW        */
    QVariantList m_currents;           /* 8 scaled channel currents (amps) */
    float m_currentMean = 0.f;
    float m_currentMax  = 0.f;         /* max |channel|, drives current warning */
    float m_vibX        = 0.f;
    float m_vibY        = 0.f;
    float m_vibZ        = 0.f;
    float m_vibTotal    = 0.f;

    /* Power window: sliding sum of per-row instantaneous power (W). */
    float *m_powerWin = nullptr;       /* ring buffer, size POWER_WINDOW_SAMPLES */
    int    m_powerHead = 0;            /* index of oldest sample                 */
    size_t m_powerCount = 0;           /* samples currently in window            */
    double m_powerSum = 0.0;           /* Σ(Va·Ia + Vb·Ib + Vc·Ic) over window   */

    /* Emit throttle. */
    QElapsedTimer m_emitTimer;
    int64_t m_lastEmitMs = 0;
    float   m_lastSpeedEmit = 0.f;
    float   m_lastPowerEmit = 0.f;

    /* AI status. */
    QString m_aiStatus     = QStringLiteral("normal");
    QString m_aiFaultClass = QStringLiteral("none");
    QString m_aiRul        = QStringLiteral("RUL: N/A");
    bool    m_aiNormal     = true;
    bool    m_aiElectrical = false;
    bool    m_aiMechanical = false;

    bool m_speedWarning   = false;
    bool m_vibWarning     = false;
    bool m_currentWarning = false;
    bool m_criticalAlert  = false;
};

#endif // CLUSTER_H
