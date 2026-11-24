# 02 — BLE

The smallest thing still recognisable as the Adelie/Hub link.

```
GUI toggle        --  BLE write         -->  LED1 on the DK
BUTTON1 on the DK --  BLE notification  -->  counter in the GUI
```

```sh
../sb ble build && ../sb ble flash
cd host && run            # Windows cmd;  ./run.sh in git bash
```

Click **Connect**, then the LED button.

## What it shows

One custom 128-bit GATT service with two characteristics — **write for
commands in, notify for data out**. That is the shape of every BLE
peripheral, and of the hub's `a88279d1` / `a88279d2`.

On the host, the threading model is the part worth reading: Qt owns the main
thread, asyncio owns another, and they talk through Qt signals and
`run_coroutine_threadsafe`. Adelie does exactly this.

## What it leaves out

Everything above the transport — here the characteristic value *is* the
message. That is what 03 adds.

| | this | Hub |
| --- | --- | --- |
| Role | peripheral | peripheral (Adelie) + central (TTL generator) |
| On disconnect | LED forced off | outputs safe, session ends |
| MTU | 247 requested | 247 requested |
