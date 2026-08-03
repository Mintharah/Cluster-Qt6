#ifndef SPIREADER_H
#define SPIREADER_H

#include <QThread>
#include <cstdint>

/* Single sensor row (mirrors motor_row_t from motor_wire.h). */
struct MotorRow {
    uint16_t current[8];  /* raw 12-bit ADC counts, 8 channels PA0..PA7   */
                          /* [0..2]=phase currents A/B/C                  */
                          /* [3..5]=phase voltages A/B/C                  */
                          /* [6]=DC bus, [7]=speed voltage                */
    int16_t  vib_x;       /* MPU6050 accel, ±2g -> 16384 counts/g         */
    int16_t  vib_y;
    int16_t  vib_z;
    uint16_t rpm;         /* already in RPM units from timer capture      */
};

/* One block (up to MOTOR_MAX_ROWS_PER_BLOCK rows) delivered to the Qt main
 * thread on every new block published by the producer (~100 Hz). */
struct MotorBlock {
    uint32_t seq;         /* producer frame seq                            */
    uint64_t timestamp;   /* microseconds (STM ticks)                     */
    uint16_t n_rows;      /* valid rows in this block                     */
    uint16_t flags;
    MotorRow rows[200];   /* MOTOR_MAX_ROWS_PER_BLOCK (motor_wire.h)      */
};
Q_DECLARE_METATYPE(MotorBlock)

/* Background thread that polls the shared-memory ring buffer produced by
 * motor_controller (MOTOR_SHM_NAME). Every time a new block is committed it
 * emits newBlock() carrying a copy of the whole block. */
class SpiReader : public QThread {
    Q_OBJECT
public:
    explicit SpiReader(QObject *parent = nullptr);
    void stop();

signals:
    void newBlock(MotorBlock data);

protected:
    void run() override;

private:
    volatile bool m_running = true;
    void *m_shm             = nullptr;
    size_t m_shm_size       = 0;
};

#endif // SPIREADER_H
