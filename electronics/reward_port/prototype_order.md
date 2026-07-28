# Required

| Item                                                                         | Qty | Farnell or RS link                                                                                                        | Note                                            |
| ---------------------------------------------------------------------------- | --: | ------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------- |
| Omron `EE-SX1140`                                                            |  1x | [Farnell 1348958](https://uk.farnell.com/omron-electronic-components/ee-sx1140/photo-interrupter-transmissive/dp/1348958) | Should be the same lightgate as on the original |
| Power Management IC Development Kit Motor Driver for DRV8837                 |  1x | [RS components 2659774](https://uk.rs-online.com/web/p/power-motor-robotics-development-tools/2659774)                    | H-Bridge breakout board                         |
| DFRobot common-anode RGB LED breakout, `DFR0239`, with straight 4-pin header |  1x | [Farnell 4308179](https://uk.farnell.com/dfrobot/dfr0239/rgb-led-breakout-5v-13x13mm/dp/4308179)                          |
| Diotec `2N7000` through-hole N-channel MOSFET                                |  4x | [Farnell 4555444](https://uk.farnell.com/diotec/2n7000/mosfet-n-channel-60v-0-2a-to-92/dp/4555444)                        |

## Kinda required

| Item                                                                             |       Qty | Farnellor RS link                                                                                               | Note                                                                                                                                        |
| -------------------------------------------------------------------------------- | --------: | --------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| Diodes Inc. PAM8302AADCR mono Class-D amplifier, SOIC-8                          |        1x | [Farnell 3373818](https://uk.farnell.com/diodes-inc/pam8302aadcr/audio-power-amp-d-40-to-85deg/dp/3373818)      | Only if doing audio                                                                                                                         |
| Aries LCQT-SOIC8-8 SOIC-8 to DIP adapter                                         |        1x | [Farnell 2476033](https://uk.farnell.com/aries/lcqt-soic8-8/ic-adaptor-8-soic-to-dip-2-54mm/dp/2476033)         | Only if doing audio                                                                                                                         |
| PUI Audio `AS02808MR-LW152-R` speaker                                            |        1x | [Farnell 4411359](https://uk.farnell.com/pui-audio/as02808mr-lw152-r/speaker-500hz-20khz-8ohm-95dba/dp/4411359) | Only if we are doing audio                                                                                                                  |
| Seeed Studio `103020193` Grove RS-485 board                                      | 2(hub)+2x | [Farnell 4007772](https://uk.farnell.com/seeed-studio/103020193/serial-comm-board-rs485-arduino/dp/4007772)     | I need at least 4 RS-485 transcievers for a full implementaion (can do it with 1+1x since sync doesn't have to be differential on the test) |
| Seeed Studio Grove 4-pin to male-jumper conversion cable, pack of 5, `110990210` |         1 | [Farnell 3410135](https://uk.farnell.com/seeed-studio/110990210/cable-4-pin-male-jumper-grove/dp/3410135)       | Connectors for transcievers because they are annoying                                                                                       |
| Multicomp Pro `MF25 120R`, 120 Ω, 1%, ¼ W through-hole resistor                  |        3x | [Farnell 9341218](https://uk.farnell.com/multicomp-pro/mf25-120r/res-120r-1-0-25w-axial-metal-film/dp/9341218)  | Not an exact necessity but they are useful for RS485 trunk termination                                                                      |
| Multicomp Pro `MC01006`, 45 × 91 mm, 2.54 mm matrix perfboard                    |        2x | [Farnell 2768276](https://uk.farnell.com/multicomp-pro/mc01006/prototype-board-phenolic-91mm/dp/2768276)        | Fire safety precaution                                                                                                                      |

# Nice to haves

| Item                                                                           | Qty | Farnell or RS link                                                                                             | Note                                                       |
| ------------------------------------------------------------------------------ | --: | -------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------- |
| Bud Industries `BC-32625` assorted-length male-to-male jumper wires, 65 pieces |   1 | [Farnell 2762505](https://uk.farnell.com/bud-industries/bc-32625/jump-wire-bundle-solderless-bread/dp/2762505) | When already ordering a bunch of wires would come in handy |
| DFRobot `FIT0121` assorted-length male-to-female jumper wires, 65 pieces       |   1 | [Farnell 3769939](https://uk.farnell.com/dfrobot/fit0121/jumper-wire-set-65pc-female-to/dp/3769939)            |
| Harwin `D01-9922046` breakaway male 2.54 mm header, 20 pins                    |   1 | [Farnell 1022217](https://uk.farnell.com/harwin/d01-9922046/conn-hdr-20pos-1row-2-54mm/dp/1022217)             |
| Samtec `SSA-120-S-T` breakaway female 2.54 mm socket strip, 20 pins            |   1 | [Farnell 3550851](https://uk.farnell.com/samtec/ssa-120-s-t/conn-rcpt-20pos-1row-2-54mm/dp/3550851)            |

# Soldering iron tips

| Item                                                | Qty | Farnell link                                                                                                 |
| --------------------------------------------------- | --: | ------------------------------------------------------------------------------------------------------------ |
| Multicomp Pro `MP740228`, 1.6 mm chisel tip         |   1 | [Farnell 3265141](https://uk.farnell.com/multicomp-pro/mp740228/soldering-tip-chisel-1-6mm/dp/3265141)       |
| Multicomp Pro `MP740229`, 2.4 mm chisel tip         |   1 | [Farnell 3265142](https://uk.farnell.com/multicomp-pro/mp740229/soldering-tip-chisel-2-4mm/dp/3265142)       |
| Multicomp Pro `MP740236`, 0.4 mm bent-point tip     |   1 | [Farnell 3265150](https://uk.farnell.com/multicomp-pro/mp740236/soldering-tip-pointed-bent-0-4mm/dp/3265150) |
| Multicomp Pro `MP740232`, 0.5 mm straight-point tip |   1 | [Farnell 3265145](https://uk.farnell.com/multicomp-pro/mp740232/soldering-tip-pointed-0-5mm/dp/3265145)      |

# Project will fall apart without these

| Item                                     | Link                                                                                                                           |
| ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------ |
| Assorted industrial communication cables | [Mouser 595-SK-AM64B ](https://www.mouser.co.uk/en/ProductDetail/Texas-Instruments/SK-AM64B?qs=dbcCsuKDzFUm4q0JHqB%2F9Q%3D%3D) |
| BLE evaluation platform                  | [Farnell 4803865](https://uk.farnell.com/ezurio/453-00197-k1/dev-board-bluetooth-low-energy/dp/4803865)                        |
| Programmable red breadboard              | [Farnell 4262565](https://uk.farnell.com/beagleboard/102110898/sbc-beaglev-fire-risc-v-fpga/dp/4262565)                        |
