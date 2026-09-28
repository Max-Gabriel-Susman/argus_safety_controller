# Running the loop end to end

**Repo:** `argus_safety_controller`. This is the runbook for bringing up the
full path — host relay → Zynq PS → PL → UDP telemetry → ROS graph — on
`argus-workstation-iii`. It spans three repos and a ROS workspace, so it lives
here, with the firmware, which is the thing being run.

It takes four terminals and Vitis. That is not incidental: each terminal owns
one process whose lifetime you need to see, and the order they start in
matters because the firmware talks to two of them within milliseconds of the
ELF loading.

```
┌────────────────────┬────────────────────┐
│ A  dataset relay   │ B  serial console  │   host → board      board → you
├────────────────────┼────────────────────┤
│ C  UDP receiver    │ D  ros2 CLI        │   board → ROS       you → ROS
└────────────────────┴────────────────────┘
                 Vitis: build, program, run
```

## One-time setup

Everything in this section survives reboots except the network address.

**ROS workspace.** All four packages built in one place:

```bash
source /opt/ros/humble/setup.bash
cd ~/Documents/argus_ws && colcon build
```

A package "not found" by `ros2 run` is a package that was not built *here* —
`ls install/` shows what is.

**Alias.** ROS and Vitis environments conflict, so neither goes in `.bashrc`.
In `~/.bash_aliases`:

```bash
alias rosenv='source /opt/ros/humble/setup.bash && source ~/Documents/argus_ws/install/setup.bash'
```

The workspace path is the one that goes stale when things move.

**Serial.** Membership of `dialout`, once, then log out and back in:

```bash
sudo usermod -aG dialout "$USER"
```

**Network.** The board is on the USB Ethernet adapter, on its own subnet. The
address does not persist and NetworkManager will take the interface back
unless told not to:

```bash
sudo nmcli device set enx00e04c685e7e managed no
sudo ip addr add 192.168.1.20/24 dev enx00e04c685e7e
sudo ip link set enx00e04c685e7e up
ip route get 192.168.1.10          # must say dev enx00e04c685e7e, not wlp0s20f3
```

If the last line names the Wi-Fi interface, the board is unreachable. NordVPN
does this: its tunnel captures 192.168.1.0/24. Disconnect it before a run.

**Data.** Raw datasets live in `~/argus_data/`, outside every repo. A
converted segment for the relay:

```bash
python3 ~/Documents/argus_ws/src/argus_sim/tools/nwb_to_replay.py \
  ~/argus_data/indy_20161005_06_broadband.nwb --start 120 --seconds 10 \
  --out ~/argus_data/indy_20161005_06_s120_10s.bin
```

**Vitis.** Launch it with the workspace on the command line, always:

```bash
vitis -w ~/Documents/argus_safety_controller
```

A bare `vitis` from some other directory sets `${workspaceFolder}` to that
directory, and `launch.json` resolves the ELF path against it. The symptom is
`Failed to download .../safety_controller/build/safety_controller.elf` with a
path missing the repo directory.

**After every gateware build**, the platform has to be told: `arty_z7_platform`
→ Settings → `vitis-comp.json` → Update XSA (same path), then build the
platform, then the app. Rebuilding the app alone reprograms the *old*
bitstream. Check before Run:

```bash
ls -l --time-style=long-iso \
  ~/Documents/argus_safety_controller/safety_controller/_ide/bitstream/argus_neural_codec.bit \
  ~/Documents/argus-neural-codec/argus_neural_codec.xsa
```

The `.bit` must be newer than the `.xsa`.

## The run, in order

### A — relay

```bash
rosenv
ros2 run argus_sim dataset_relay_node --ros-args \
  -p dataset_path:=$HOME/argus_data/indy_20161005_06_s120_10s.bin
```

Omit `dataset_path` to serve the synthetic identity pattern instead — the
frame checks in the firmware expect it, and it is the right choice when you
are debugging the transport rather than the data. Healthy:

```
[INFO] [dataset_relay]: dataset: /home/prometheus/argus_data/indy_20161005_06_s120_10s.bin
[INFO] [dataset_relay]: replay server on 0.0.0.0:5010 -- 300093 samples x 96 channels, 7 samples/chunk, loop=true
```

**Before Vitis Run.** The firmware sends its first fetch request within
milliseconds of starting; with no relay listening it times out, prints
`replay FAILED`, and streaming never begins.

### B — serial console

```bash
screen /dev/ttyUSB1 115200
```

The Arty enumerates two FTDI ports; the console is the higher-numbered one.
The screen goes blank and stays blank — that is attached and waiting.

**Before Vitis Run.** The banner prints as the ELF starts. Attaching after
means an empty screen with no way to tell healthy from hung.

### C — receiver

```bash
rosenv
ros2 run argus_sensors neural_udp_receiver
```

Prints `No frames received on :5005` every 5 s until the board is up. That
is the correct idle state. It can start any time; nothing on the board waits
for it.

### Vitis — Run

Component `safety_controller` → **Run**. The XSDB console should show
`fpga -file ... argus_neural_codec.bit` at 100%, `ps7_init`, a processor
reset, and `dow` of the ELF succeeding.

### D — ros2 CLI

```bash
rosenv
ros2 topic echo /argus/neural_interface_bridge/neural_data --once
```

Anything else you want to look at from the ROS side goes here too:
`ros2 topic hz`, `ros2 topic echo /dataset_relay/status`.

### E — decoder

```bash
rosenv
ARGUS_DATASET_PATH=$HOME/argus_data/indy_20161005_06.mat \
  ros2 run argus_inference inference_node --ros-args \
  -p input_topic:=/argus/neural_interface_bridge/neural_data
```

Trains on the `.mat` at startup (~1 s), logs `offline 4-way intent accuracy:
0.440`, then decodes every frame the fabric sends and publishes `/cmd_vel`.
One log line per second, `sample` advancing by 20. In D, `ros2 topic hz
/cmd_vel` at 20 Hz is the end of the pipeline.

## What healthy looks like

**Console (B)**, in order. The first line is the one to read:

```
Initializing Argus Safety Controller...
acq id=41435133                       <- fabric revision matches firmware
acq status=00000001 frames/s=30012    <- sweep rate
acq timing: reg 1080 ns/read, 96-word frame read 114 us, sweep 33 us (read is held)
acq frame N: [0]=.... [95]=.... idx=.. expect=.. bad=0     <- identity pattern only
acq features: bin 3  ch0 count 0 power 4  ch14 count 0 power 5  dropped 0
Configuring PHY for fixed 1000 Mbps mode
link speed for phy address 1: 1000
replay clock: 10 ms measured over 10 ms sleep
replay ok: [0][0]=0000 [0][5]=0500 [1][0]=0001 [146][95]=9F92
replay: req=1 rtx=0 ok=21 rej=0 to=0 chunks=21/21
stream: priming
Argus Safety Controller initialized. IP 192.168.1.10
tx 20 bin 13 skipped 0                <- one line per second, bin +20, skipped stays 0
```

Then every ~5 s a `stream:` line, a `feat:` line and an `acq frame` line:

```
stream: halves=172 underruns=3561 failures=0 next=25284 pl: half=1 row=64 c0=1 c1=1
feat: bin 493 dropped 0  ch14 count 0 power 4572  ch75 count 5 power 11024
acq frame 739935: [0]=7FB9 [95]=7D2E idx=B9 (ext) bad=96
```

`dropped` stays 0 and power sits in the thousands. The frame line's `bad=96`
is the identity-pattern check running on real data — expected, not a fault.
`tx` and `bin` differ by a few: `bin` restarts when streaming enters ext mode.

The first line is the one that matters. `acq id=41435132  EXPECTED 41435133
-- stale bitstream?` means the platform still carries the previous fabric;
the firmware then parks after `acq features: hold never took effect` and
sends nothing. See the one-time Vitis section.

**Receiver (C):**

```
frames ok=92 size=0 magic=0 ver=0 crc=0
```

About 92 per 5 s window, the four failure counters at 0.

**Echo (D):** `channel_count: 96` and 96 small integers — crossing counts per
50 ms bin, mostly 0 with single digits on the live channels. `sample` is the
bin number and `t` is `sample × 0.04998`.

**Decoder (E):** `offline 4-way intent accuracy: 0.440`, then one line per
second with `sample` advancing by 20 and an `intent -> vx wz` pair.

## Known state, as of 2026-09-27 (tag `acq3-live`)

- **The pipeline is complete.** Replayed cortex → simulated chips → SPI →
  crossings and spike-band power in fabric → UDP → DDS → LDA → `/cmd_vel`
  at 20.0 Hz. Telemetry is paced by the fabric's bin counter now, so the
  rate is exact.
- **`underruns` climbs, `halves` barely moves.** The PS refills BRAM halves
  over AXI-Lite one word at a time, about 7 ms per half against the 4.9 ms
  the fabric takes to play one. It keeps up with ~3% of them; the rest
  replay stale, so the codec is computing on a stutter of the same 5 ms of
  recording. Counts are structurally right and numerically meaningless; the
  decoder's intents are noise. The fix is DMA from DDR to BRAM, and it is
  now the only thing between this pipeline and real output.
- **Power is computed but not sent.** `NeuralFrame` carries counts only;
  the `feat:` line shows power. Putting it on the wire is the next firmware
  and message change, and it is what takes the decoder from 44% to 53%.

## Shutting down

In this order, so nothing is left holding a port or a JTAG channel:

1. **Vitis:** stop the debug session (Debug view → red square), or File →
   Exit. Pulling the USB cable under a live session wedges XSDB.
2. **Console:** `Ctrl-A` then `k`, then `y`. *Not* `Ctrl-A d`, and not
   closing the window — a detached `screen` still owns `/dev/ttyUSB1` and the
   next attach shows nothing.
3. **A, C, D:** `Ctrl-C`.

## When it does not work

| Symptom | Cause | Fix |
| --- | --- | --- |
| `Package 'argus_sensors' not found` | not built in `~/Documents/argus_ws` | `colcon build`, see one-time setup |
| `.../argus_ws/install/setup.bash: No such file` | `rosenv` points at an old workspace | fix the path in `~/.bash_aliases` |
| `topic ... does not appear to be published yet` | receiver (C) not running | start it |
| `replay FAILED ... to=6 chunks=0/21` | relay (A) not up before Run | start A, reset the board |
| `replay FAILED` with A up | VPN tunnel has the subnet | `ip route get 192.168.1.10`; disconnect NordVPN |
| `acq id=41435131  EXPECTED 41435132 -- stale bitstream?` | fabric predates firmware | `build_bitstream.tcl`, re-read XSA, rebuild platform + app |
| `acq frame: hold never took effect` | same, older firmware | same |
| `Failed to download .../Documents/safety_controller/build/...elf` | Vitis launched without `-w` | relaunch `vitis -w ~/Documents/argus_safety_controller` |
| `Failed to connect to target 127.0.0.1:3121 Reason: abort` | previous XSDB session wedged | File → Exit, `pkill -f hw_server; pkill -f xsdb`, relaunch |
| `Opening COM4: Access denied` / blank console after attach | a `screen` is still attached elsewhere | `screen -ls`, `screen -X -S <id> quit` |
| `[screen is terminating]` immediately | same: a stale session holds the port | `screen -X -S "$(screen -ls \| awk '/tty\|pts/ {print $1; exit}')" quit`, then attach |
| Console attached but silent for minutes, receiver sees nothing | firmware parked on a stale fabric | `ls -l` the `.bit` vs the `.xsa`; Update XSA, build platform, build app |
| `The message type 'argus_core/msg/NeuralFrame' is invalid` | shell has base ROS but not the workspace overlay | `rosenv` in that shell |
| `Package 'argus_inference' not found` with the package built | same | `rosenv` |
| `Destination Host Unreachable` from ping | no route on the USB NIC | one-time network setup again |
| `tx ... skipped N` with N > 0 | hold failing on the fabric | should not happen on ACQ2; report it |
| Receiver `ver=` climbing, `ok=0` | host `argus_wire.h` behind the board's | sync from `argus_core`, rebuild `argus_sensors` |

## Optional: one command for the three ROS terminals

If `tmux` is installed, this opens A, C and D in one window, sourced, with A
and C already running:

```bash
tmux new-session -d -s argus \
  "bash -ic 'rosenv && ros2 run argus_sim dataset_relay_node --ros-args -p dataset_path:=\$HOME/argus_data/indy_20161005_06_s120_10s.bin'" \; \
  split-window -v "bash -ic 'rosenv && ros2 run argus_sensors neural_udp_receiver'" \; \
  split-window -h "bash -ic 'rosenv; exec bash'" \; \
  select-layout tiled \; attach
```

The console stays in its own terminal — `screen` inside `tmux` works but the
`Ctrl-A` prefix collides with tmux's default and has to be remapped.
