# Korora prototype

Prototype wiring for the Korora hub using the hardware currently on hand:

- Nordic nRF52840 DK
- ST STEVAL-MKI240KA / LSM6DSV32X 6-axis IMU
- MEMSIC MMC5983MA-B / MMC5983MA 3-axis magnetometer
- Existing Fairy communication hardware and firmware

Both motion sensors connect directly to the nRF52840 over one shared I2C bus.

The nPM power system is not part of this prototype wiring revision. During sensor bring-up, the nRF52840 DK supplies the sensor boards from its 3.3 V rail.

## Prototype architecture

```text
                              existing Fairy hardware
                                      |
                                      |
                         UART / SYNC / EVENT / TTL
                                      |
                                      v
                         +-------------------------+
                         |      nRF52840 DK        |
                         |                         |
                         | Fairy UART              |
                         | TX  P0.27               |
                         | RX  P0.26               |
                         |                         |
                         | Korora sensor I2C       |
                         | SDA P0.30 / A4          |
                         | SCL P0.31 / A5          |
                         +-----+--------------+----+
                               |              |
                         shared I2C      shared I2C
                               |              |
                               v              v
                    +----------------+   +----------------+
                    | STEVAL-        |   | MMC5983MA-B    |
                    | MKI240KA       |   |                |
                    | LSM6DSV32X     |   | MMC5983MA      |
                    | accel + gyro   |   | magnetometer   |
                    | addr 0x6A      |   | addr 0x30      |
                    +----------------+   +----------------+

                         optional interrupt GPIOs

                    IMU INT1 -------> P0.28 / A2
                    MAG INT  -------> P0.29 / A3
```

## nRF52840 DK sensor pin assignment

| Function      | nRF GPIO | DK header name     | Use                                       |
| ------------- | -------- | ------------------ | ----------------------------------------- |
| I2C SDA       | P0.30    | A4                 | shared sensor SDA                         |
| I2C SCL       | P0.31    | A5                 | shared sensor SCL                         |
| IMU INT1      | P0.28    | A2                 | optional/recommended data-ready interrupt |
| MAG INT       | P0.29    | A3                 | optional/recommended data-ready interrupt |
| Sensor supply | 3V3      | 3.3 V power header | both eval boards                          |
| Sensor ground | GND      | GND                | common ground                             |

A2 and A3 are only used for interrupt outputs. All sensor register communication remains I2C.

## Shared I2C bus

Both sensor boards are connected in parallel:

```text
nRF P0.30 / A4 / SDA
        |
        +---------- STEVAL-MKI240KA SDA
        |
        +---------- MMC5983MA-B SDA


nRF P0.31 / A5 / SCL
        |
        +---------- STEVAL-MKI240KA SCL
        |
        +---------- MMC5983MA-B SCL
```

Common supply:

```text
nRF DK 3V3
   |
   +---------- STEVAL-MKI240KA VDD
   +---------- STEVAL-MKI240KA VDDIO
   +---------- MMC5983MA-B VDD

nRF DK GND
   |
   +---------- STEVAL-MKI240KA GND
   +---------- MMC5983MA-B GND
```

## STEVAL-MKI240KA

Sensor:

```text
LSM6DSV32X
3-axis accelerometer
3-axis gyroscope
```

For this prototype it is used only as a normal I2C peripheral.

Do not use the LSM6DSV32X sensor-hub pins to attach the magnetometer. The magnetometer is connected directly to the nRF52840 on the same I2C bus.

### I2C mode configuration

For I2C operation:

```text
CS       -> 3V3 / VDDIO
SDO/SA0  -> GND
```

With `SDO/SA0` low, the selected 7-bit address is:

```text
0x6A
```

The alternative address with `SDO/SA0` high is `0x6B`, but use `0x6A` for this prototype so the wiring and firmware are deterministic.

### STEVAL-MKI240KA wiring

Use the signal names printed on the board / DIL24 adapter.

| STEVAL-MKI240KA signal | Connect to nRF52840 DK  | Purpose                                   |
| ---------------------- | ----------------------- | ----------------------------------------- |
| VDD                    | 3V3                     | sensor supply                             |
| VDDIO                  | 3V3                     | digital I/O supply                        |
| GND                    | GND                     | common ground                             |
| SDA                    | A4 / P0.30              | shared I2C SDA                            |
| SCL                    | A5 / P0.31              | shared I2C SCL                            |
| CS                     | 3V3                     | selects I2C interface                     |
| SDO/SA0                | GND                     | selects address 0x6A                      |
| INT1                   | A2 / P0.28              | optional/recommended data-ready interrupt |
| INT2                   | not connected initially | reserve for later use                     |
| SDx                    | not connected initially | sensor-hub / auxiliary function not used  |
| SCx                    | not connected initially | sensor-hub / auxiliary function not used  |
| OCS / OCS_Aux          | not connected initially | auxiliary interface not used              |
| SDO_Aux                | not connected initially | auxiliary interface not used              |

## MMC5983MA-B

Sensor:

```text
MMC5983MA
3-axis magnetometer
```

The MMC5983MA-B prototyping board exposes two four-pin headers.

### I2C mode configuration

For I2C operation, SPI chip select must be held high:

```text
P1-1 SPI_CS -> 3V3
```

The MMC5983MA uses the fixed 7-bit I2C address:

```text
0x30
```

This does not conflict with the IMU at `0x6A`.

### MMC5983MA-B wiring

| MMC5983MA-B pin | Signal  | Connect to nRF52840 DK          |
| --------------- | ------- | ------------------------------- |
| P1-1            | SPI_CS  | 3V3                             |
| P1-2            | NC      | no connection                   |
| P1-3            | VDD     | 3V3                             |
| P1-4            | GND     | GND                             |
| P2-1            | SPI_SDO | no connection                   |
| P2-2            | SDA     | A4 / P0.30                      |
| P2-3            | SCL     | A5 / P0.31                      |
| P2-4            | INT     | A3 / P0.29 optional/recommended |

## Complete wiring table

| nRF52840 DK | nRF GPIO | STEVAL-MKI240KA | MMC5983MA-B |
| ----------- | -------- | --------------- | ----------- |
| 3V3         | -        | VDD             | P1-3 VDD    |
| 3V3         | -        | VDDIO           | P1-1 SPI_CS |
| GND         | -        | GND             | P1-4 GND    |
| A4          | P0.30    | SDA             | P2-2 SDA    |
| A5          | P0.31    | SCL             | P2-3 SCL    |
| A2          | P0.28    | INT1            | -           |
| A3          | P0.29    | -               | P2-4 INT    |
| GND         | -        | SDO/SA0         | -           |
| 3V3         | -        | CS              | -           |

## Zephyr overlay

The existing Fairy overlay should be preserved.

Use the following merged overlay for the prototype:

```dts
#include <zephyr/dt-bindings/gpio/gpio.h>
#include <zephyr/dt-bindings/pinctrl/nrf-pinctrl.h>
#include <zephyr/dt-bindings/i2c/i2c.h>

/ {
    zephyr,user {
        /*
         * Existing Fairy signals.
         * Keep these assignments unchanged.
         */
        sync-gpios = <&gpio1 10 GPIO_ACTIVE_LOW>;
        event-gpios = <&gpio1 11 GPIO_ACTIVE_HIGH>;
        ttl-input-gpios = <&gpio0 2 GPIO_ACTIVE_HIGH>;

        /*
         * Korora sensor interrupt inputs.
         * Sensor register communication itself is entirely I2C.
         */
        imu-int-gpios = <&gpio0 28 GPIO_ACTIVE_HIGH>;
        mag-int-gpios = <&gpio0 29 GPIO_ACTIVE_HIGH>;
    };
};

&pinctrl {
    /*
     * Existing Fairy UART.
     */
    fairy_uart1_default: fairy_uart1_default {
        group1 {
            psels = <NRF_PSEL(UART_TX, 0, 27)>;
        };

        group2 {
            psels = <NRF_PSEL(UART_RX, 0, 26)>;
            bias-pull-up;
        };
    };

    fairy_uart1_sleep: fairy_uart1_sleep {
        group1 {
            psels = <NRF_PSEL(UART_TX, 0, 27)>,
                    <NRF_PSEL(UART_RX, 0, 26)>;
            low-power-enable;
        };
    };
};

&uart1 {
    compatible = "nordic,nrf-uarte";
    status = "okay";
    current-speed = <460800>;
    pinctrl-0 = <&fairy_uart1_default>;
    pinctrl-1 = <&fairy_uart1_sleep>;
    pinctrl-names = "default", "sleep";
};

/*
 * The DK normally maps i2c0 onto P0.26/P0.27.
 *
 * Fairy owns those pins, so i2c0 must be disabled when
 * CONFIG_I2C is enabled.
 */
&i2c0 {
    status = "disabled";
};

/*
 * The DK board definition says i2c1 and spi1 cannot be used
 * together. Korora uses this peripheral instance for I2C.
 */
&spi1 {
    status = "disabled";
};

/*
 * nRF52840 DK default i2c1 pinctrl:
 *
 * SDA = P0.30 = A4
 * SCL = P0.31 = A5
 */
&i2c1 {
    status = "okay";
    clock-frequency = <I2C_BITRATE_STANDARD>;
};

&timer2 {
    status = "okay";
};

&timer3 {
    status = "okay";
};

&rtc2 {
    status = "okay";
};
```

No sensor child nodes are required for the first electrical bring-up.

The first firmware test can talk to the devices through the normal Zephyr I2C API using their addresses directly:

```text
LSM6DSV32X = 0x6A
MMC5983MA  = 0x30
```

This avoids making the prototype depend on the exact sensor-driver support present in a particular Zephyr / nRF Connect SDK version.

Once the raw bus is proven, proper driver/devicetree integration can be added separately.

## Interrupt wiring

The sensors can initially be polled over I2C.

However, the recommended final prototype wiring includes:

```text
LSM6DSV32X INT1 -> P0.28 / A2
MMC5983MA INT    -> P0.29 / A3
```

This allows later firmware to timestamp data-ready events and avoids making the acquisition timing depend on a polling loop.

Do not enable GPIO interrupts until normal I2C reads are working.

### IMU interrupt

Use INT1 initially for accelerometer / gyroscope data-ready or FIFO watermark.

The exact routing is configured inside the LSM6DSV32X registers.

### Magnetometer interrupt

The MMC5983MA INT output can be used for measurement/data-ready operation when configured.

It is not required for the first I2C communication test.

## Sensor acquisition strategy

The first implementation should keep acquisition simple:

```text
nRF52840
   |
   +-- read LSM6DSV32X accel + gyro
   |
   +-- read MMC5983MA magnetometer
   |
   +-- timestamp data in Korora hub time
   |
   +-- fairy event generated
```

Do not use the LSM6DSV32X embedded sensor hub for the MMC5983MA in this prototype.

The nRF52840 should own both sensors directly.

## Magnetometer placement during prototype testing

Keep the MMC5983MA-B away from:

- speakers
- solenoids
- steel tools
- bench supply transformers
- large current loops
- the nRF DK USB cable if it carries significant nearby current
- magnets
- ferromagnetic breadboard hardware

For early software development it can sit on jumper wires away from the DK.

This will make calibration and heading tests much more meaningful.

## 9-DoF output goal

The combined prototype provides:

```text
LSM6DSV32X:
    acceleration X/Y/Z
    angular rate X/Y/Z

MMC5983MA:
    magnetic field X/Y/Z
```

giving nine measured axes.

Target application update rate:

```text
20-100 Hz
```

The sensors do not by themselves provide drift-free long-term XYZ position.

## Existing Fairy timing

The existing Fairy timing resources remain:

```text
timer2 = enabled
timer3 = enabled
rtc2   = enabled
```

Sensor acquisition should timestamp samples using the same Korora timing model rather than creating an unrelated timebase.

The detailed timestamp / fusion scheduling policy can be added after raw sensor data are proven.

## Prototype-to-PCB decisions this setup should answer

This prototype should provide enough evidence to freeze several Korora PCB decisions:

- whether one shared I2C bus is sufficient
- final I2C speed
- whether both data-ready interrupt lines are useful
- required sensor sample rates
- sensor-fusion CPU load
- practical fused output rate
- IMU filtering settings
- magnetometer calibration approach
- required physical separation between magnetometer and noisy hardware
- final sensor-axis orientation on the Korora PCB

The custom Korora PCB can then use the same logical topology:
