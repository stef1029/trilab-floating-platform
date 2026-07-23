# Synchronization and command protocol

## Scope

This document defines the current communication and logging interfaces between Korora, Fairy, Galapagos, and Adelie.

It covers:

- The shared 40 byte remote record frame
- Fairy I2C transactions
- Galapagos Bluetooth notifications
- Adelie 20 byte control frames
- Korora stream schema version 3
- Clock model and event matching rules
- Concurrency and data integrity requirements

All numeric wire fields use little endian byte order unless stated otherwise.

## Public node names

The canonical names are:

```text
korora
fairy
galapagos
adelie
```

Older logs may contain `reward_port`. Parsers provided normalize that legacy name to `fairy`.

## Shared remote frame

Fairy and Galapagos use the same 40 byte frame format.

Frame constants:

```text
magic 0xA5
version 2
length 40
```

### Byte layout

Offset 0

Size 1

Field `magic`

Required value `0xA5`

Offset 1

Size 1

Field `version`

Required value `2`

Offset 2

Size 1

Field `status_flags`

Offset 3

Size 1

Field `length`

Required value `40`

Offset 4

Size 1

Field `record_type`

Offset 5

Size 1

Field `pending_count`

Offset 6

Size 1

Field `record_flags`

Offset 7

Size 1

Field `reserved`

Required value `0`

Offset 8

Size 8

Field `capture_ticks`

Offset 16

Size 8

Field `snapshot_ticks`

Offset 24

Size 4

Field `capture_loss_count`

Offset 28

Size 4

Field `transport_error_count`

Offset 32

Size 4

Field `auxiliary`

Offset 36

Size 2

Field `crc`

Offset 38

Size 2

Field `reserved_tail`

Required value `0`

### CRC

The CRC is CRC 16 CCITT with polynomial `0x1021` and initial value `0xFFFF`.

The CRC covers bytes 0 through 35.

The CRC field itself and the final reserved bytes are not included.

A receiver must reject a frame when any of these checks fail:

- Magic is not `0xA5`
- Version is not `2`
- Length is not `40`
- CRC does not match
- Reserved values violate the current version rules

## Status flags

Bit 0

Name `RECORD_VALID`

Meaning a record is present

Bit 1

Name `FIRST_AFTER_RESET`

Meaning this is the first valid report after reset or connection state reset

Bit 2

Name `CAPTURE_LOSS_LATCHED`

Meaning a hardware overcapture, queue overflow, or dropped report was observed

Bit 3

Name `TRANSPORT_ERROR_LATCHED`

Meaning an I2C or Bluetooth transport error was observed

Bit 4

Name `CLOCK_FAULT`

Meaning the source clock or capture path entered a fault state

Bits 5 through 7 are reserved.

## Record flags

Bit 0 is currently used by Fairy to report hardware overcapture for the associated timer channel.

Galapagos currently sends zero record flags.

All unknown record flag bits must be preserved in raw logs and ignored by older receivers unless the protocol version changes their meaning.

## Record types

Value `0`

Name `NONE`

Meaning no record

Value `1`

Name `SYNC`

Meaning synchronization observation

Value `2`

Name `EVENT`

Meaning external GPIO event

Value `3`

Name `COMMAND_ACK`

Meaning Fairy command acknowledgement

## Common timestamp fields

### capture_ticks

This is the source timestamp associated with the record.

For Fairy it is a 64 bit TIM2 timestamp in a 16 MHz domain.

For a Galapagos synchronization record it is the selected Bluetooth anchor timestamp multiplied by 16.

For a Galapagos event record it is the GRTC hardware capture timestamp multiplied by 16.

### snapshot_ticks

This is the source clock value when the frame was built or the record snapshot was created.

The source transport age is the difference between snapshot ticks and capture ticks.

For a modulo free 64 bit counter this requires snapshot ticks to be at or after capture ticks.

The receiver must treat impossible ordering as a malformed or stale frame.

### capture_loss_count

This is a cumulative source counter.

Fairy increments it for timer overcapture or FIFO overflow.

Galapagos increments it when a report cannot be queued or is discarded because the link is unavailable.

### transport_error_count

This is a cumulative source counter.

Fairy uses it for non address failure I2C errors.

Galapagos uses it for Bluetooth notification failures and related transport failures.

### pending_count

This is queue occupancy associated with the returned or transmitted record.

It is not a synchronization ID.

It is not an event ID.

It must not be used for matching.

## Auxiliary field

The 32 bit auxiliary field depends on source and record type.

### Fairy synchronization and event records

Existing Fairy firmware may place boot reset status or another source diagnostic value here.

Korora must not assume that this field is an event sequence unless the Fairy firmware version explicitly defines it.

Current Korora analysis may assign a synthetic increasing event ID to Fairy event records.

### Fairy command acknowledgement

The command token is carried in the low seven bits.

The acknowledgement must match the active command token in Korora.

### Galapagos synchronization record

The low 16 bits contain the Bluetooth connection event counter.

Korora unwraps and matches this counter to its own controller anchor observation.

### Galapagos event record

The field contains the Galapagos external event sequence.

The sequence increases once for each accepted GPIO capture.

## Fairy I2C transport

Korora is the I2C controller.

Fairy is the target at unshifted address:

```text
0x42
```

### Read transaction

Korora reads exactly 40 bytes.

Fairy presents one atomic snapshot of the oldest queued record.

A complete successful target transmit removes that record from the queue.

An aborted transfer leaves the record queued for retry.

Korora validates magic, version, length, reserved values, and CRC before using the frame.

### Fairy FIFO

The current Fairy FIFO contains 16 records.

Synchronization, event, and command acknowledgement records share the transport.

Queue overflow increments the capture loss count and can latch a clock fault.

### Fairy synchronization pairing

Fairy does not need to know the Korora pulse number.

Korora assigns the pulse identity using its own hardware pulse schedule and the acceptance window.

The pairing process is:

1. Korora emits a hardware synchronization pulse

2. Fairy captures the edge

3. Korora polls Fairy

4. Korora checks frame integrity

5. Korora checks transport age and pulse phase

6. Korora holds at most one valid candidate for the pulse window

7. Korora admits the candidate after the window closes

8. Korora sends the accepted pair to the Fairy clock model

A second valid synchronization candidate in the same acceptance window invalidates that window.

A stale synchronization record is drained and reported but is not assigned to a new pulse.

Event and command acknowledgement records are handled independently from synchronization admission.

### Fairy command bytes

Value `0x01`

Meaning acknowledge the first after reset status

Value `0x02`

Meaning clear latched capture and transport status flags while preserving cumulative counters

Values with bit 7 set

Meaning execute the immediate command

For an immediate command:

```text
command byte equals 0x80 combined with token
```

The token uses the low seven bits.

Example:

```text
sequence 10000
token 16
command byte 0x90
```

Fairy records command receipt, executes the action in thread context, and queues a command acknowledgement record.

The action must not run inside the I2C interrupt callback.

## Galapagos Bluetooth transport

Galapagos is a Bluetooth peripheral.

Korora is the Bluetooth central for this link.

The advertised name is:

```text
galapagos
```

### Service UUID values

The synchronization service uses the encoded value:

```text
A88278D0 7009 4BEE A6F8 E1DC3FF02B92
```

The anchor report characteristic uses:

```text
A88278D2 7009 4BEE A6F8 E1DC3FF02B92
```

The characteristic supports notifications.

### Notification payload

Each notification contains exactly one 40 byte shared remote frame.

Galapagos sends synchronization and event records through one outgoing queue.

The notification path must permit only one active notification at a time.

The required design is:

```text
record producer
→ outgoing message queue
→ notification thread
→ bt_gatt_notify_cb
→ completion callback
→ next record
```

The frame buffer must remain valid until notification completion.

Temporary memory exhaustion or retry conditions may be retried after a short delay.

A disconnect or disabled notification state should purge stale queued records.

### Galapagos anchor report

The SoftDevice Controller provides:

- Connection handle
- Connection event counter
- Anchor timestamp in microseconds

Galapagos selects reports according to the configured report cadence.

The current target report rate is approximately 1 Hz.

The transmitted local timestamp is the anchor microsecond value multiplied by 16.

The connection event counter is placed in the low 16 bits of the auxiliary field.

### Galapagos hardware event capture

The overlay defines P1.11 as the event input.

The capture path is:

```text
GPIO rising edge
→ GPIOTE input event
→ DPPI or GPPI connection
→ GRTC capture task
→ GRTC compare register
```

GRTC runs at 1 MHz.

The interrupt handler reads the already captured compare value and queues a record.

The transmitted local timestamp is the captured microsecond value multiplied by 16.

The interrupt handler must not use a mutex, wait for Bluetooth, or perform serial logging.

## Adelie control frame

Adelie and Korora use a separate 20 byte application frame.

Frame constants:

```text
magic 0xAD1E
version 1
length 20
```

### Byte layout

Offset 0

Size 2

Field `magic`

Offset 2

Size 1

Field `version`

Offset 3

Size 1

Field `message_type`

Offset 4

Size 4

Field `sequence`

Offset 8

Size 8

Field `timestamp`

Offset 16

Size 4

Field `value`

All fields use little endian order.

The frame fits inside one ATT operation at the default ATT MTU.

## Adelie request types

Value `0x01`

Name `CLOCK_SYNC_REQUEST`

Meaning request one Korora clock exchange reply

Value `0x02`

Name `FAIRY_DO_NOW_REQUEST`

Meaning execute an immediate Fairy command

## Adelie notification types

Value `0x81`

Name `CLOCK_REPLY`

Timestamp meaning Korora receive time

Value meaning Korora reply queue time difference from receive time

Value `0x90`

Name `COMMAND_KORORA_RX`

Meaning Korora received the Adelie command

Value `0x91`

Name `COMMAND_FAIRY_TX_START`

Meaning Korora started the Fairy I2C command transfer

Value `0x92`

Name `COMMAND_FAIRY_RX`

Meaning Fairy received the command

Value `0x93`

Name `COMMAND_FAIRY_EXEC`

Meaning Fairy completed the immediate action timestamp

Value `0x94`

Name `COMMAND_KORORA_ACK_RX`

Meaning Korora received the Fairy acknowledgement

Value `0x95`

Name `COMMAND_KORORA_DONE_TX`

Meaning Korora queued or submitted the final result notification

Value `0xFF`

Name `ERROR`

Meaning the request failed

## Adelie status values

Value `0`

Name `OK`

Value `1`

Name `BUSY`

Value `2`

Name `BAD_FRAME`

Value `3`

Name `I2C_WRITE_FAILED`

Value `4`

Name `TIMEOUT`

Value `5`

Name `FAIRY_MODEL_INVALID`

Value `6`

Name `TOKEN_MISMATCH`

Value `7`

Name `NOTIFY_FAILED`

## Adelie clock exchange

Adelie records four logical timestamps.

`t1`

Adelie time immediately before the write

`t2`

Korora time in the receive callback

`t3`

Korora time immediately before the reply is queued

`t4`

Adelie time in the notification callback

The network round trip is the full Adelie elapsed interval after removing Korora processing time.

The clock pair uses the midpoint of the Adelie interval and the midpoint of the Korora interval.

The rolling model uses the latest 16 accepted exchanges.

The current application performs repeated exchanges in the background and refits after each accepted point.

One way latency values are estimates because they depend on the fitted model and path symmetry.

## Adelie command sequence

A normal command progresses through:

1. Adelie command transmit

2. Korora command receive

3. Korora Fairy transfer start

4. Fairy command receive

5. Fairy command action complete

6. Korora acknowledgement receive

7. Korora result queue

8. Korora result transmit submission

9. Adelie result receive

The final end to end round trip uses Adelie send time and Adelie receive time.

Internal stage times use Korora and Fairy hardware timestamp domains after conversion where required.

The 0x95 timestamp is the Korora result queue or submission time. It is not guaranteed to be the physical Bluetooth radio transmit start.

## Korora stream schema version 3

Korora begins the machine readable stream with:

```text
SCHEMA,3
```

A parser must switch explicitly on the schema version.

A parser must not guess a column layout after an unsupported schema value.

Every machine readable record is CSV.

For node qualified records, the second field is the node name.

## PAIR_RAW record

Format:

```text
PAIR_RAW,node,sync_id,hub_ticks,local_ticks,prospective_count,has_previous,local_delta_ticks,local_interval_error_ticks,transport_age_ticks
```

Field meanings:

`node`

Remote node name

`sync_id`

Fairy pulse number or unwrapped Galapagos connection event counter

`hub_ticks`

Korora timestamp associated with the pair

`local_ticks`

Remote timestamp

`prospective_count`

Number of samples that would exist after admitting this point

`has_previous`

One when an adjacent previous pair is available

`local_delta_ticks`

Local interval since the previous accepted pair

`local_interval_error_ticks`

Difference between measured local interval and the nominal interval

`transport_age_ticks`

Age reported by the source frame

## SYNC record

Format:

```text
SYNC,node,sync_id,hub_ticks,local_ticks,status_flags,record_flags,pending_count,state,slope_ppb,local_reference_ticks,hub_reference_ticks,rms_ns,prefit_residual_ns,model_step_ns,transport_age_ticks
```

Field meanings:

`status_flags`

Raw remote status flags

`record_flags`

Raw per record flags

`pending_count`

Remote queue occupancy

`state`

`ACQUIRE` or `TRACK`

`slope_ppb`

Affine slope error in parts per billion

`local_reference_ticks`

Local reference point for the affine model

`hub_reference_ticks`

Korora value at the reference point

`rms_ns`

Current model residual RMS

`prefit_residual_ns`

Residual of the new point before refitting

`model_step_ns`

Change in model prediction produced by the proposed update

## EVENT record

Format:

```text
EVENT,node,event_id,kind,local_ticks,local_hz,hub_ticks,state,transport_age_ticks
```

Field meanings:

`event_id`

Identity assigned by the source or by Korora

`kind`

Event stage name

`local_ticks`

Timestamp in the node local domain

`local_hz`

Frequency of the local timestamp domain

`hub_ticks`

Timestamp expressed in Korora time when available

`state`

Conversion state

`transport_age_ticks`

Age in the source local domain

### Event states

`LOCAL`

The timestamp is already in Korora time

`TRACK`

A valid model converted the timestamp into Korora time

`UNSYNC`

No valid model was available

`REMOTE`

Korora received the timestamp but does not own the source conversion model

For `UNSYNC`, the hub timestamp sentinel is outside the valid time range and must not be plotted as a real timestamp.

### Event kinds

Current kinds include:

- `GPIO_RISE`
- `CLOCK_SYNC_TX`
- `CLOCK_SYNC_RX`
- `CLOCK_SYNC_REPLY_QUEUE`
- `CLOCK_SYNC_REPLY_TX`
- `COMMAND_TX`
- `COMMAND_RX`
- `COMMAND_FORWARD`
- `COMMAND_EXEC`
- `COMMAND_ACK_RX`
- `COMMAND_RESULT_QUEUE`
- `COMMAND_RESULT_TX`

## EVENT_MATCH record

Format:

```text
EVENT_MATCH,node,event_id,reference_node,reference_event_id,converted_hub_ticks,reference_hub_ticks,error_ns
```

Korora emits this after matching a converted remote GPIO event to a cached Korora GPIO event.

The signed error is the converted remote event time relative to the Korora reference time.

A positive error means the converted remote event is later than the Korora reference event.

A match is valid only when:

- The remote model is in `TRACK`
- The converted timestamp is valid
- A Korora reference event exists inside the configured match window
- The reference event ID is greater than zero

Zero reference IDs are invalid sentinels and must not be included in matched event plots.

## LINK record

Format:

```text
LINK,node,transport,role,state,peer,interval_us,reason
```

This describes connection and transport state.

Typical states include advertising, scanning, connected, disconnected, subscribed, and parameter updated.

## FAULT record

Format:

```text
FAULT,node,category,code,value
```

This reports failures and counters that require diagnostic attention.

The code may contain an operating system or transport error value.

## READY record

Format:

```text
READY,node,timer_hz,sync_period_ticks,fairy_i2c_address,model_window,max_skew_ppm,galapagos_name,event_pin
```

The current Korora implementation reports its key runtime configuration in this row.

## Clock model rules

Each remote model uses a rolling window of 16 accepted pairs.

The model is affine and uses a local reference point.

A proposed point may be rejected because of:

- Invalid frame
- Invalid pulse or anchor identity
- Excess transport age
- Implausible adjacent interval
- Excess residual
- Excess RMS
- Excess model step
- Repeated admission rejection
- Capture or transport fault

A model reset invalidates timestamp conversion until acquisition succeeds again.

The analysis must begin a new segment after each model reset.

## Event matching rules

Korora captures its own GPIO event and stores it in a bounded cache.

A remote GPIO event is converted only when the associated remote model is valid.

Korora chooses the nearest cached reference event inside the configured time window.

The remote `event_id` and Korora `reference_event_id` remain separate identities.

The event error is reported in nanoseconds.

The matching process must not reuse queue occupancy, pending count, or transport age as an event identifier.

## Serial settings

Korora VCOM0 uses:

```text
115200 baud
8 data bits
no parity
1 stop bit
no software flow control
no hardware flow control
```

A host recorder should open the actual VCOM0 device, preferably through a stable symbolic link.

Only one host program may own the port at a time.

Host timestamp prefixes should remain disabled for logs that will be parsed as the raw firmware stream.

## Compatibility

The wire values in the Adelie protocol retain compatibility aliases for older reward terminology.

The public node name for new output is `fairy`.

The shared 40 byte frame remains version 2.

The Korora text stream is version 3.

Any incompatible change to field meaning, length, or ordering requires a version increase.

## Validation checklist

- Confirm `SCHEMA,3` appears once at startup
- Confirm one complete CSV record per line
- Confirm the parser reports no merged records
- Confirm all 40 byte frames pass CRC
- Confirm Fairy and Galapagos reach `TRACK`
- Confirm local and remote event IDs are nonzero
- Confirm every valid match has a positive reference ID
- Confirm Galapagos event timestamps come from GRTC hardware capture
- Confirm notification buffers remain valid until completion
- Confirm no interrupt handler performs blocking work
- Confirm model resets begin new analysis segments
- Confirm VCOM0 is opened as 115200 8N1
