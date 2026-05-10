#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(scd41, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>

#include "topic.h"
#include "transport.h"
#include <zephyr/drivers/i2c.h>

#define SCD_NODE DT_NODELABEL(scd41_0)
#if !DT_NODE_HAS_STATUS(SCD_NODE, okay)
#error "SCD41 devicetree node 'scd41_0' missing or not OK"
#endif

static const struct device *const scd = DEVICE_DT_GET(SCD_NODE);


#define SCD41_I2C_ADDR              0x62
#define SCD41_CMD_STOP_MEAS         0x3F86
#define SCD41_CMD_FRC               0x362F
#define SCD41_CMD_START_MEAS        0x21B1
 
#define T_STOP_MS                   500
#define T_FRC_MS                    400


#define SCD41_WARMUP_MS 5000

static inline int32_t to_centi(const struct sensor_value *v)
{
    int64_t micro = v->val2;
    int64_t centi = ((int64_t)v->val1 * 100)
                  + (micro >= 0 ? (micro + 5000) / 10000
                                : (micro - 5000) / 10000);
    return (int32_t)centi;
}

static void publish_field(const char *root, const char *sensor,
                          const char *field, int32_t value)
{
    char topic[64];
    char payload[24];

    if (topic_build(topic, sizeof(topic), root, "OUT", sensor, field) != 0) {
        return;
    }

    snprintk(payload, sizeof(payload), "%d", (int)value);
    int rc = transport_publish(topic, payload);

    if (rc < 0) {
        LOG_WRN("PUB %s failed rc=%d", topic, rc);
    } else {
        LOG_INF("PUB %s OK", topic);
    }
}

static struct {
    bool    warmup_done;
    int64_t warmup_deadline;
} scd_ctx;

void scd41_sample_and_publish(const char *root)
{
    if (!device_is_ready(scd)) {
        static bool warned = false;
        if (!warned) {
            LOG_WRN("SCD41: device NOT ready (check DT, I2C, power)");
            warned = true;
        }
        return;
    }

    int64_t now = k_uptime_get();

    if (!scd_ctx.warmup_done && scd_ctx.warmup_deadline == 0) {
        LOG_INF("SCD41: starting warm-up (%d ms)", SCD41_WARMUP_MS);
        scd_ctx.warmup_deadline = now + SCD41_WARMUP_MS;
    }

    if (!scd_ctx.warmup_done && now < scd_ctx.warmup_deadline) {
        struct sensor_value dummy;
        sensor_sample_fetch(scd);
        sensor_channel_get(scd, SENSOR_CHAN_CO2, &dummy);
        return;
    }

    scd_ctx.warmup_done = true;

    if (sensor_sample_fetch(scd) != 0) {
        LOG_WRN("SCD41: sample_fetch failed");
        return;
    }

    struct sensor_value co2, t, rh;
    if (sensor_channel_get(scd, SENSOR_CHAN_CO2, &co2)        ||
        sensor_channel_get(scd, SENSOR_CHAN_AMBIENT_TEMP, &t) ||
        sensor_channel_get(scd, SENSOR_CHAN_HUMIDITY, &rh)) {
        LOG_WRN("SCD41: channel_get failed");
        return;
    }

    uint32_t co2_ppm  = (co2.val1 > 0) ? (uint32_t)co2.val1 : 0u;
    int32_t  t_centi  = to_centi(&t);
    int32_t  rh_centi = to_centi(&rh);

    LOG_INF("SCD41: CO2=%u ppm, T=%d.%02d C, RH=%d.%02d %%",
            co2_ppm,
            t_centi / 100,  abs(t_centi % 100),
            rh_centi / 100, abs(rh_centi % 100));

    publish_field(root, "SCD41-1", "CO2",  (int32_t)co2_ppm);
    publish_field(root, "SCD41-1", "TEMP", t_centi);
    publish_field(root, "SCD41-1", "RH",   rh_centi);
}


/* Sensirion CRC-8 (poly 0x31, init 0xFF) */
static uint8_t scd41_crc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31)
                               : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static const struct device *scd41_get_i2c_bus(void)
{
    /* Zelfde I2C bus als de Zephyr driver gebruikt — via DT alias */
    return DEVICE_DT_GET(DT_BUS(DT_NODELABEL(scd41_0)));
}

static int scd41_send_cmd(const struct device *i2c, uint16_t cmd)
{
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    return i2c_write(i2c, buf, sizeof(buf), SCD41_I2C_ADDR);
}

int scd41_forced_recalibration(uint16_t reference_ppm)
{
    /* Niet uitvoeren tijdens warmup */
    if (!scd_ctx.warmup_done) {
        LOG_WRN("SCD41 FRC: deferred, warmup not done");
        return -EAGAIN;
    }
        
    const struct device *i2c = scd41_get_i2c_bus();
    if (!device_is_ready(i2c)) {
        LOG_ERR("SCD41 FRC: I2C bus not ready");
        return -ENODEV;
    }

    /* 1. Stop periodieke meting */
    int rc = scd41_send_cmd(i2c, SCD41_CMD_STOP_MEAS);
    if (rc < 0) {
        LOG_ERR("SCD41 FRC: stop_meas failed: %d", rc);
        return rc;
    }
    k_msleep(T_STOP_MS);

    /* 2. Stuur FRC commando met referentie ppm + CRC */
    uint8_t frc_buf[5];
    frc_buf[0] = (uint8_t)(SCD41_CMD_FRC >> 8);
    frc_buf[1] = (uint8_t)(SCD41_CMD_FRC & 0xFF);
    frc_buf[2] = (uint8_t)(reference_ppm >> 8);
    frc_buf[3] = (uint8_t)(reference_ppm & 0xFF);
    frc_buf[4] = scd41_crc(&frc_buf[2], 2);

    rc = i2c_write(i2c, frc_buf, sizeof(frc_buf), SCD41_I2C_ADDR);
    if (rc < 0) {
        LOG_ERR("SCD41 FRC: frc_cmd failed: %d", rc);
        goto restart;
    }
    k_msleep(T_FRC_MS);

    /* 3. Lees FRC correctie resultaat (2 bytes + CRC) */
    uint8_t resp[3];
    rc = i2c_read(i2c, resp, sizeof(resp), SCD41_I2C_ADDR);
    if (rc < 0) {
        LOG_ERR("SCD41 FRC: read result failed: %d", rc);
        goto restart;
    }

    if (scd41_crc(resp, 2) != resp[2]) {
        LOG_ERR("SCD41 FRC: CRC mismatch");
        rc = -EBADMSG;
        goto restart;
    }

    /* FRC correctie waarde: 0xFFFF = mislukt */
    uint16_t correction = ((uint16_t)resp[0] << 8) | resp[1];
    if (correction == 0xFFFF) {
        LOG_ERR("SCD41 FRC: sensor reported failure (0xFFFF)");
        rc = -EIO;
        goto restart;
    }

    /* Correctie in ppm = correction - 32768 */
    int16_t corr_ppm = (int16_t)(correction - 32768);
    LOG_INF("SCD41 FRC: OK ref=%u ppm, correction=%d ppm", reference_ppm, corr_ppm);
    rc = 0;

restart:
    /* 4. Herstart periodieke meting altijd, ook bij fout */
    int rc2 = scd41_send_cmd(i2c, SCD41_CMD_START_MEAS);
    if (rc2 < 0) {
        LOG_ERR("SCD41 FRC: start_meas failed: %d", rc2);
    }

    /* Reset warmup zodat app_core de warmup opnieuw afwacht */
    scd_ctx.warmup_done     = false;
    scd_ctx.warmup_deadline = 0;

    return rc;
}
