//! The streaming state machine, `argus_replay.c`'s with read-ahead: keeps
//! the PL's ping-pong BRAM fed from the relay through a ring of halves in
//! DDR.
//!
//! The PL plays one half while the other waits its turn. It reports a half
//! consumed when it flips out of it; the PS loads that half with the oldest
//! half in the ring, makes it visible to the PL (a cache clean, since the
//! aperture is mapped write-back) and acks. If the PL flips back into a half
//! before its ack, it sets underrun and keeps playing stale data; that is
//! counted, not fatal.
//!
//! Meanwhile the PS fetches ahead, one fetch at a time, into the ring's free
//! slots, up to [`SLOTS`] halves beyond what the PL has been given. A relay
//! that answers late then draws the ring down instead of costing an
//! underrun, as long as it catches up before the ring runs dry. In v2.0
//! each fetch landed straight in the half the PL had freed, so one fetch
//! slower than about 4.4 ms was an underrun.
//!
//! The stream primes by fetching until the ring is full, loads the two
//! oldest halves into half 0 and half 1, then puts the chain into external
//! mode with a soft reset, so playback starts at row 0 of half 0, and runs.
//! A failed fetch is retried at the same offset into the same slot. The PL
//! is given the dataset in order: each half the 147 samples after the last.
//!
//! Nothing here touches the hardware or the network. The firmware hands in
//! the PL ([`Pl`]: REPLAY_STATUS, REPLAY_ACK, CTRL and loading a half from
//! the ring), the fetch ([`Fetch`]: the replay client and its socket) and a
//! microsecond [`Clock`], so the whole machine runs in host tests against a
//! fake PL.

use argus_replay::State as FetchState;

use crate::acq::{ReplayStatus, ctrl, replay_ack};
use crate::lines::{StreamFetch, StreamHalves, StreamRx};
use crate::replay::SAMPLES_PER_HALF;
use crate::ring::{Ring, SLOTS};
use crate::timing::Timing;

// Priming loads two halves out of the ring before anything else comes in.
const _: () = assert!(SLOTS >= 2, "the ring must hold both halves to prime");

/// One half of the ping-pong BRAM.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Half {
    /// Bytes 0 to 28223 of the aperture; played first.
    Zero,
    /// Bytes 28224 to 56447.
    One,
}

impl Half {
    /// 0 or 1, the C's `half` and the index into the aperture.
    pub const fn index(self) -> usize {
        match self {
            Self::Zero => 0,
            Self::One => 1,
        }
    }

    /// The REPLAY_ACK bit that hands this half back to the PL.
    const fn ack(self) -> u32 {
        match self {
            Self::Zero => replay_ack::CONSUMED0,
            Self::One => replay_ack::CONSUMED1,
        }
    }

    /// Whether `status` reports this half consumed, awaiting a load.
    const fn is_consumed(self, status: ReplayStatus) -> bool {
        match self {
            Self::Zero => status.consumed0(),
            Self::One => status.consumed1(),
        }
    }
}

/// What the stream needs of the PL.
pub trait Pl {
    /// Reads REPLAY_STATUS.
    fn replay_status(&mut self) -> ReplayStatus;

    /// Writes REPLAY_ACK: each set bit clears its flag.
    fn ack_replay(&mut self, bits: u32);

    /// Writes CTRL whole.
    fn write_ctrl(&mut self, value: u32);

    /// Copies ring slot `slot` into `half` and makes it visible to the PL,
    /// which reads the BRAM directly and never sees the A9's caches. Must
    /// be complete when it returns: the half is acked right after.
    fn load_half(&mut self, half: Half, slot: usize);
}

/// What the stream needs of the replay client.
pub trait Fetch {
    /// Begins fetching one half's worth of samples ([`SAMPLES_PER_HALF`])
    /// from dataset sample `offset`. The firmware lands the samples in the
    /// ring slot [`Stream::in_flight`] names. Returns whether the request
    /// left.
    fn start(&mut self, offset: u32) -> bool;

    /// Where the fetch stands. The stream waits for `Complete` or `Failed`.
    fn state(&self) -> FetchState;
}

/// A microsecond clock that wraps at 2^32, the C's `argus_now_us()`.
pub trait Clock {
    /// Microseconds since some fixed moment.
    fn now_us(&self) -> u32;
}

/// `stream_state_t`, with priming one phase now that it fills a ring.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Phase {
    /// Not begun.
    Off,
    /// Fetching until the ring is full.
    Prime,
    /// Loading whichever half the PL has finished with, and fetching ahead.
    Run,
}

/// The stream: where it is, what it has counted since it began, and the
/// timing figures since the last report.
#[derive(Clone, Debug)]
pub struct Stream {
    phase: Phase,
    /// Which slots of the firmware's ring hold fetched halves.
    ring: Ring<SLOTS>,
    /// Dataset offset of the next fetch, up to a ring's worth ahead of what
    /// the PL plays.
    next_sample: u32,
    /// The ring slot being fetched into, if any.
    in_flight: Option<usize>,

    /// Halves loaded into the PL.
    halves_loaded: u32,
    underruns: u32,
    failures: u32,

    /// When the fetch in flight was requested.
    fetch_started_us: u32,
    /// Request to last chunk, per fetch.
    fetch: Timing,
    /// Loading a half into the PL: the copy from the ring and the clean.
    flush: Timing,
    /// When the last fetch while running ended, until the next request
    /// leaves; `None` while priming, so priming is not counted.
    done_us: Option<u32>,
    /// Fetch done to next request sent, while running. Includes waiting for
    /// a free slot and whatever else the firmware does meanwhile.
    gap: Timing,
}

impl Default for Stream {
    fn default() -> Self {
        Self::new()
    }
}

impl Stream {
    /// A stream that has not begun.
    pub const fn new() -> Self {
        Self {
            phase: Phase::Off,
            ring: Ring::new(),
            next_sample: 0,
            in_flight: None,
            halves_loaded: 0,
            underruns: 0,
            failures: 0,
            fetch_started_us: 0,
            fetch: Timing::new(),
            flush: Timing::new(),
            done_us: None,
            gap: Timing::new(),
        }
    }

    /// `argus_replay_stream_begin()`: resets the counters and the ring and
    /// requests the first half from `first_sample`. If the request does not
    /// leave, the next [`service`](Self::service) tries again.
    pub fn begin<F: Fetch, C: Clock>(&mut self, first_sample: u32, fetch: &mut F, clock: &C) {
        *self = Self::new();
        self.next_sample = first_sample;
        self.phase = Phase::Prime;
        self.start_fetch(fetch, clock);
    }

    /// The ring slot being fetched into, which is where the firmware must
    /// land the chunks that arrive; `None` between fetches.
    pub const fn in_flight(&self) -> Option<usize> {
        self.in_flight
    }

    /// Whether priming is over and the PL is playing the halves.
    pub fn is_running(&self) -> bool {
        self.phase == Phase::Run
    }

    /// `argus_replay_stream_service()`, after the client has been polled:
    /// moves the stream on. Call it often; the PL consumes a half every
    /// 4.9 ms.
    ///
    /// While running, in this order, because the PL's deadline is the hard
    /// one and a fetch has a ring's worth of slack: an underrun is counted
    /// and acked; a half the PL has consumed is loaded from the ring; then
    /// the fetch side moves on.
    pub fn service<P: Pl, F: Fetch, C: Clock>(&mut self, pl: &mut P, fetch: &mut F, clock: &C) {
        match self.phase {
            Phase::Off => {}
            Phase::Prime => {
                self.fill(fetch, clock);
                if self.ring.is_full() {
                    self.start_playing(pl, clock);
                }
            }
            Phase::Run => {
                let status = pl.replay_status();
                if status.underrun() {
                    self.underruns = self.underruns.wrapping_add(1);
                    pl.ack_replay(replay_ack::UNDERRUN);
                }
                self.play(status, pl, clock);
                self.fill(fetch, clock);
            }
        }
    }

    /// The end of priming: the ring is full, so the two oldest halves go
    /// into half 0 and half 1, and the PL starts playing them.
    ///
    /// The low-water mark is taken once here: it fell to 0 while the ring
    /// was being filled, and the first report should show the fewest seen
    /// while running.
    fn start_playing<P: Pl, C: Clock>(&mut self, pl: &mut P, clock: &C) {
        for half in [Half::Zero, Half::One] {
            if let Some(slot) = self.ring.oldest() {
                self.load(half, slot, pl, clock);
            }
        }
        enter_ext_mode(pl);
        self.phase = Phase::Run;
        self.ring.take_low_water();
    }

    /// The play side: if the half the PL plays next (the one it is not
    /// playing) is consumed, loads it with the oldest half in the ring and
    /// acks it. With the ring empty it stays unacked, still flagged, and is
    /// loaded by the first call after a fetch completes.
    ///
    /// Both halves are flagged only after an underrun, the PL playing one
    /// of them stale. That one is left alone until the PL moves out of it:
    /// the fabric flags a half consumed every time it leaves it, so a half
    /// loaded and acked while it plays would be flagged again and loaded
    /// over before its samples were played, and the PL would skip them.
    fn play<P: Pl, C: Clock>(&mut self, status: ReplayStatus, pl: &mut P, clock: &C) {
        let next = match status.play_half() {
            0 => Half::One,
            _ => Half::Zero,
        };
        if !next.is_consumed(status) {
            return;
        }
        let Some(slot) = self.ring.oldest() else {
            return;
        };
        self.load(next, slot, pl, clock);
        pl.ack_replay(next.ack());
    }

    /// Loads ring slot `slot`, the oldest, into `half`, times it and frees
    /// the slot. The caller acks.
    fn load<P: Pl, C: Clock>(&mut self, half: Half, slot: usize, pl: &mut P, clock: &C) {
        let t0 = clock.now_us();
        pl.load_half(half, slot);
        self.flush.record(clock.now_us().wrapping_sub(t0));
        // `slot` is the oldest, so this releases the slot just loaded.
        self.ring.release();
        self.halves_loaded = self.halves_loaded.wrapping_add(1);
    }

    /// The fetch side: sees the fetch in flight through, then starts the
    /// next one if a slot is free.
    ///
    /// A completed fetch commits its slot, which becomes the newest, and
    /// moves the dataset offset on. A failed one counts a failure and
    /// leaves the slot free and the offset where it was, so the fetch is
    /// retried by the next call at the same offset into the same slot.
    fn fill<F: Fetch, C: Clock>(&mut self, fetch: &mut F, clock: &C) {
        if self.in_flight.is_some() {
            let now = clock.now_us();
            match fetch.state() {
                FetchState::Complete => {
                    self.fetch.record(now.wrapping_sub(self.fetch_started_us));
                    // The slot in flight is the ring's next free one, and
                    // only this commits, so the commit always succeeds.
                    self.ring.commit();
                    self.next_sample = self.next_sample.wrapping_add(u32::from(SAMPLES_PER_HALF));
                    self.fetch_done(now);
                }
                FetchState::Failed => {
                    self.failures = self.failures.wrapping_add(1);
                    self.fetch_done(now);
                    return;
                }
                FetchState::Idle | FetchState::Waiting => return,
            }
        }
        self.start_fetch(fetch, clock);
    }

    /// The fetch in flight is over, at `now`, one way or the other.
    fn fetch_done(&mut self, now: u32) {
        self.in_flight = None;
        if self.phase == Phase::Run {
            self.done_us = Some(now);
        }
    }

    /// `stream_fetch_into()`: requests the next block into the ring's next
    /// free slot, if one is free and nothing is in flight. A request that
    /// does not leave counts as a failure and leaves nothing in flight, so
    /// the next call tries again.
    fn start_fetch<F: Fetch, C: Clock>(&mut self, fetch: &mut F, clock: &C) {
        if self.in_flight.is_some() {
            return;
        }
        let Some(slot) = self.ring.next_free() else {
            return;
        };
        if !fetch.start(self.next_sample) {
            self.failures = self.failures.wrapping_add(1);
            return;
        }
        self.in_flight = Some(slot);
        self.fetch_started_us = clock.now_us();
        if let Some(done_us) = self.done_us.take() {
            self.gap.record(self.fetch_started_us.wrapping_sub(done_us));
        }
    }

    /// The `stream: halves=` line: the counts since the stream began, and
    /// `status`, REPLAY_STATUS read for the report.
    pub const fn halves_line(&self, status: ReplayStatus) -> StreamHalves {
        StreamHalves {
            halves: self.halves_loaded,
            underruns: self.underruns,
            failures: self.failures,
            next: self.next_sample,
            pl: status,
        }
    }

    /// The fetch, flush and gap figures since the last report, and starts
    /// them again from none.
    pub fn take_timings(&mut self) -> StreamTimings {
        StreamTimings {
            fetch: self.fetch.take(),
            flush: self.flush.take(),
            gap: self.gap.take(),
        }
    }

    /// The fewest filled ring slots since the last call, and starts the
    /// count again from the current fill.
    pub fn take_ring_min(&mut self) -> usize {
        self.ring.take_low_water()
    }
}

/// `stream_enter_ext_mode()`: soft reset with ext_mode set, clear every
/// replay flag, then release with ext_mode kept. Playback starts at row 0
/// of half 0 with no partial first frame.
///
/// CTRL is written whole, as in the C, so this clears any hold bit; the
/// firmware never calls it while a hold is in progress.
fn enter_ext_mode<P: Pl>(pl: &mut P) {
    pl.write_ctrl(ctrl::SOFT_RESET | ctrl::EXT_MODE);
    pl.ack_replay(replay_ack::CONSUMED0 | replay_ack::CONSUMED1 | replay_ack::UNDERRUN);
    pl.write_ctrl(ctrl::ENABLE | ctrl::EXT_MODE);
}

/// The stream's timing figures over one report interval.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StreamTimings {
    /// Request to last chunk, per fetch.
    pub fetch: Timing,
    /// Loading a half into the PL, per half.
    pub flush: Timing,
    /// Fetch done to next request sent, while running.
    pub gap: Timing,
}

/// The receive path's figures over one report interval: the C's
/// `g_copy_us_*`, `g_parse_us_*`, `g_rx_n` and `g_loop_*`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RxStats {
    /// Moving a datagram out of the socket.
    copy: Timing,
    /// The client's checks, CRC and copy into the ring slot.
    parse: Timing,
    /// Wakeups of the replay task.
    loop_iters: u32,
    /// Datagrams those wakeups delivered.
    loop_pkts: u32,
}

impl RxStats {
    /// No datagrams yet.
    pub const fn new() -> Self {
        Self {
            copy: Timing::new(),
            parse: Timing::new(),
            loop_iters: 0,
            loop_pkts: 0,
        }
    }

    /// One datagram: `copy_us` to take it out of the socket, `parse_us` for
    /// the client to check and place it.
    pub fn datagram(&mut self, copy_us: u32, parse_us: u32) {
        self.copy.record(copy_us);
        self.parse.record(parse_us);
    }

    /// One wakeup of the replay task, which delivered `packets` datagrams:
    /// the C's `argus_replay_note_loop()`.
    pub fn wakeup(&mut self, packets: u32) {
        self.loop_iters = self.loop_iters.wrapping_add(1);
        self.loop_pkts = self.loop_pkts.wrapping_add(packets);
    }

    /// The figures so far, starting again from none.
    pub fn take(&mut self) -> Self {
        core::mem::take(self)
    }
}

/// The three `stream:` lines of `argus_replay_stream_report()`, in order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StreamReport {
    /// `stream: halves=...`
    pub halves: StreamHalves,
    /// `stream: fetch avg=...`
    pub fetch: StreamFetch,
    /// `stream: rx n=...`
    pub rx: StreamRx,
}

/// What the report needs from outside the stream: REPLAY_STATUS, the
/// client's counters and the console's figures, all read at report time.
#[derive(Clone, Copy, Debug)]
pub struct ReportInputs {
    /// REPLAY_STATUS.
    pub status: ReplayStatus,
    /// The replay client's counters since boot.
    pub client: argus_replay::Counters,
    /// Console messages dropped since boot.
    pub con_drops: u32,
    /// The console's longest message since the last report.
    pub con_max: u32,
}

impl StreamReport {
    /// `argus_replay_stream_report()`: the lines for now, after which every
    /// timing figure, and the ring's low-water mark, starts again.
    pub fn take(stream: &mut Stream, rx: &mut RxStats, inputs: &ReportInputs) -> Self {
        let timings = stream.take_timings();
        let ring_min = stream.take_ring_min();
        let rx = rx.take();
        Self {
            halves: stream.halves_line(inputs.status),
            fetch: StreamFetch {
                fetch: timings.fetch,
                flush: timings.flush,
                retransmits: inputs.client.retransmits_sent,
                timeouts: inputs.client.timeouts,
                rejected: inputs.client.chunks_rejected,
                ring_min,
                ring_slots: SLOTS,
            },
            rx: StreamRx {
                copy: rx.copy,
                parse: rx.parse,
                loop_iters: rx.loop_iters,
                loop_pkts: rx.loop_pkts,
                gap: timings.gap,
                con_drops: inputs.con_drops,
                con_max: inputs.con_max,
            },
        }
    }
}

#[cfg(test)]
mod tests {
    extern crate std;

    use core::cell::Cell;
    use std::format;
    use std::vec::Vec;

    use super::*;

    const PLAY_HALF: u32 = 1 << 0;
    const CONSUMED0: u32 = 1 << 1;
    const CONSUMED1: u32 = 1 << 2;
    const UNDERRUN: u32 = 1 << 3;

    /// Samples per half, as the offsets step.
    const STEP: u32 = SAMPLES_PER_HALF as u32;

    /// The dataset offsets of the first `n` halves, in order.
    fn in_order(n: u32) -> Vec<u32> {
        (0..n).map(|i| i * STEP).collect()
    }

    /// How many halves `v` holds, for [`in_order`].
    fn len_u32<T>(v: &[T]) -> u32 {
        u32::try_from(v.len()).unwrap_or(u32::MAX)
    }

    /// What the fake PL saw, in order.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum Op {
        Ctrl(u32),
        Ack(u32),
        Load(Half, usize),
    }

    /// A PL whose REPLAY_STATUS the test sets or [`flip`](Self::flip)s, and
    /// which records every write. Acks clear the matching status bits, as
    /// the fabric's do.
    ///
    /// Instead of samples, the ring slots and the BRAM halves hold the
    /// dataset offset fetched into them, so the tests can follow what the
    /// PL is given and what it plays.
    #[derive(Default)]
    struct FakePl {
        status: u32,
        ops: Vec<Op>,
        /// The offset each ring slot was last fetched from.
        slots: [Option<u32>; SLOTS],
        /// The offset each BRAM half was last loaded from.
        bram: [Option<u32>; 2],
        /// Every offset loaded into the BRAM, in order.
        loaded: Vec<u32>,
        /// What the PL played, a half at a time: `None` for a half played
        /// stale after an underrun.
        played: Vec<Option<u32>>,
    }

    impl FakePl {
        /// One half period of the fabric: the PL flags the half it was
        /// playing consumed and flips to the other, and if that one has not
        /// been acked it flags an underrun and plays it stale.
        fn flip(&mut self) {
            let leaving = self.status & PLAY_HALF;
            let entering = leaving ^ 1;
            self.status |= CONSUMED0 << leaving;
            self.status = (self.status & !PLAY_HALF) | entering;
            if self.status & (CONSUMED0 << entering) != 0 {
                self.status |= UNDERRUN;
                self.played.push(None);
            } else {
                let half = usize::try_from(entering).unwrap_or(0);
                self.played.push(self.bram.get(half).copied().flatten());
            }
        }
    }

    impl Pl for FakePl {
        fn replay_status(&mut self) -> ReplayStatus {
            ReplayStatus(self.status)
        }
        fn ack_replay(&mut self, bits: u32) {
            self.ops.push(Op::Ack(bits));
            // REPLAY_ACK bit n clears REPLAY_STATUS bit n + 1.
            self.status &= !(bits << 1);
        }
        fn write_ctrl(&mut self, value: u32) {
            self.ops.push(Op::Ctrl(value));
            // The release from the soft reset: playback starts in half 0.
            if value == ctrl::ENABLE | ctrl::EXT_MODE {
                self.status &= !PLAY_HALF;
                self.played.push(self.bram[0]);
            }
        }
        fn load_half(&mut self, half: Half, slot: usize) {
            self.ops.push(Op::Load(half, slot));
            let offset = self.slots.get(slot).copied().flatten();
            if let Some(offset) = offset {
                self.loaded.push(offset);
            }
            if let Some(dst) = self.bram.get_mut(half.index()) {
                *dst = offset;
            }
        }
    }

    /// A fetch the test completes or fails by hand; records each offset
    /// requested, and refuses to send while `refuse` is set.
    struct FakeFetch {
        state: FetchState,
        offsets: Vec<u32>,
        refuse: bool,
    }

    impl FakeFetch {
        fn new() -> Self {
            Self {
                state: FetchState::Idle,
                offsets: Vec::new(),
                refuse: false,
            }
        }
    }

    impl Fetch for FakeFetch {
        fn start(&mut self, offset: u32) -> bool {
            if self.refuse {
                return false;
            }
            self.offsets.push(offset);
            self.state = FetchState::Waiting;
            true
        }
        fn state(&self) -> FetchState {
            self.state
        }
    }

    /// A clock the test moves.
    struct FakeClock(Cell<u32>);

    impl Clock for FakeClock {
        fn now_us(&self) -> u32 {
            self.0.get()
        }
    }

    impl FakeClock {
        fn advance(&self, us: u32) {
            self.0.set(self.0.get().wrapping_add(us));
        }
    }

    struct Rig {
        stream: Stream,
        pl: FakePl,
        fetch: FakeFetch,
        clock: FakeClock,
    }

    impl Rig {
        fn new() -> Self {
            Self {
                stream: Stream::new(),
                pl: FakePl::default(),
                fetch: FakeFetch::new(),
                clock: FakeClock(Cell::new(1000)),
            }
        }
        fn begin(&mut self) {
            self.stream.begin(0, &mut self.fetch, &self.clock);
        }
        fn service(&mut self) {
            self.stream
                .service(&mut self.pl, &mut self.fetch, &self.clock);
        }
        /// The fetch in flight lands its offset in its slot and completes,
        /// as the firmware's chunks land where `in_flight()` says; then the
        /// stream is serviced.
        fn complete(&mut self) {
            if let Some(slot) = self.stream.in_flight()
                && let Some(dst) = self.pl.slots.get_mut(slot)
            {
                *dst = self.fetch.offsets.last().copied();
            }
            self.fetch.state = FetchState::Complete;
            self.service();
        }
        /// Begins and primes the ring; the stream is then running and the
        /// PL playing half 0.
        fn primed() -> Self {
            let mut rig = Self::new();
            rig.begin();
            for _ in 0..SLOTS {
                rig.complete();
            }
            rig.pl.ops.clear();
            rig
        }
        /// One half period: the stream is serviced a few times, a fetch
        /// completing each time if `answering`, then the PL flips.
        fn period(&mut self, answering: bool) {
            for _ in 0..4 {
                if answering && self.fetch.state == FetchState::Waiting {
                    self.complete();
                } else {
                    self.service();
                }
            }
            self.pl.flip();
        }
        fn halves(&self) -> StreamHalves {
            self.stream.halves_line(ReplayStatus(0))
        }
    }

    #[test]
    fn a_new_stream_does_nothing() {
        let mut rig = Rig::new();
        rig.service();
        assert!(rig.fetch.offsets.is_empty());
        assert!(rig.pl.ops.is_empty());
        assert_eq!(rig.stream.in_flight(), None);
    }

    #[test]
    fn priming_fills_the_ring_loads_two_halves_then_enters_ext_mode() {
        let mut rig = Rig::new();
        rig.begin();
        assert_eq!(rig.fetch.offsets, [0]);
        assert_eq!(rig.stream.in_flight(), Some(0));

        // Nothing changes while the fetch is in progress.
        rig.service();
        assert_eq!(rig.fetch.offsets, [0]);

        // Each completed fetch starts the next, into the next slot.
        for i in 1..SLOTS {
            rig.complete();
            assert_eq!(rig.stream.in_flight(), Some(i));
            assert!(rig.pl.ops.is_empty());
            assert!(!rig.stream.is_running());
        }
        rig.complete();
        assert_eq!(rig.fetch.offsets, in_order(16));
        // Full: nothing more to fetch until the PL frees a half.
        assert_eq!(rig.stream.in_flight(), None);
        assert!(rig.stream.is_running());
        // The two oldest into half 0 and half 1, then
        // stream_enter_ext_mode, register for register. The primed halves
        // are not acked one by one.
        assert_eq!(
            rig.pl.ops,
            [
                Op::Load(Half::Zero, 0),
                Op::Load(Half::One, 1),
                Op::Ctrl(ctrl::SOFT_RESET | ctrl::EXT_MODE),
                Op::Ack(0b111),
                Op::Ctrl(ctrl::ENABLE | ctrl::EXT_MODE),
            ]
        );
        assert_eq!(rig.pl.loaded, [0, 147]);
        assert_eq!(rig.halves().halves, 2);
        assert_eq!(rig.halves().next, 16 * 147);
        // The mark starts from the fill once running, not the empty ring.
        assert_eq!(rig.stream.take_ring_min(), SLOTS - 2);
    }

    #[test]
    fn running_loads_the_consumed_half_from_the_ring_and_acks_it() {
        let mut rig = Rig::primed();

        // Nothing consumed: the ring has two free slots, so it refills.
        rig.service();
        assert!(rig.pl.ops.is_empty());
        assert_eq!(rig.fetch.offsets.last(), Some(&(16 * 147)));
        assert_eq!(rig.stream.in_flight(), Some(0));
        rig.complete();
        assert_eq!(rig.stream.in_flight(), Some(1));
        rig.complete();
        assert_eq!(rig.stream.in_flight(), None);

        // The PL leaves half 0: it gets slot 2, the oldest, then its ack.
        rig.pl.flip();
        rig.service();
        assert_eq!(
            rig.pl.ops,
            [Op::Load(Half::Zero, 2), Op::Ack(replay_ack::CONSUMED0)]
        );
        assert_eq!(rig.pl.status, PLAY_HALF);
        // And the freed slot is fetched into in the same call.
        assert_eq!(rig.stream.in_flight(), Some(2));
        assert_eq!(rig.fetch.offsets.last(), Some(&(18 * 147)));
        assert_eq!(rig.pl.loaded, in_order(3));
        assert_eq!(rig.halves().halves, 3);
    }

    #[test]
    fn with_both_consumed_only_the_half_played_next_is_loaded() {
        let mut rig = Rig::primed();
        rig.pl.status = CONSUMED0 | CONSUMED1 | PLAY_HALF;
        rig.service();
        // Half 1 is being played stale; it waits until the PL leaves it.
        assert_eq!(
            rig.pl.ops,
            [Op::Load(Half::Zero, 2), Op::Ack(replay_ack::CONSUMED0)]
        );
        rig.service();
        assert_eq!(rig.pl.ops.len(), 2);

        // The PL moves into half 0; now half 1 is the one played next.
        rig.pl.flip();
        rig.service();
        assert_eq!(
            rig.pl.ops.get(2..),
            Some(&[Op::Load(Half::One, 3), Op::Ack(replay_ack::CONSUMED1)][..])
        );

        let mut rig = Rig::primed();
        rig.pl.status = CONSUMED0 | CONSUMED1;
        rig.service();
        assert_eq!(rig.pl.ops.first(), Some(&Op::Load(Half::One, 2)));
    }

    #[test]
    fn a_consumed_half_waits_unacked_while_the_ring_is_empty() {
        let mut rig = Rig::primed();
        // Hold every fetch until the ring is dry and both halves wait.
        while rig.pl.status & (CONSUMED0 | CONSUMED1) != CONSUMED0 | CONSUMED1 {
            rig.period(false);
        }
        rig.pl.ops.clear();
        rig.service();
        // Only the underrun is acked.
        assert_eq!(rig.pl.ops, [Op::Ack(replay_ack::UNDERRUN)]);

        // The first fetch to complete goes into the half played next.
        let next = rig.halves().next;
        rig.complete();
        rig.service();
        let playing = rig.pl.status & PLAY_HALF;
        let expected = if playing == 0 { Half::One } else { Half::Zero };
        assert_eq!(rig.pl.ops.get(1).copied(), Some(Op::Load(expected, 0)));
        assert_eq!(rig.pl.loaded.last(), Some(&next));
    }

    #[test]
    fn the_pl_is_fed_the_dataset_in_order_over_hundreds_of_halves() {
        let mut rig = Rig::primed();
        for _ in 0..400 {
            rig.period(true);
        }
        let line = rig.halves();
        assert_eq!((line.underruns, line.failures), (0, 0));
        // The two primed, and one for each flip but the last, whose half
        // is loaded at the next service.
        assert_eq!(line.halves, 401);
        assert_eq!(rig.pl.loaded, in_order(401));
        // What the PL played: the priming half 0, then one half a period.
        let played: Vec<_> = in_order(401).into_iter().map(Some).collect();
        assert_eq!(rig.pl.played, played);
        // Never fetched twice, never skipped.
        assert_eq!(rig.fetch.offsets, in_order(line.next / STEP));
    }

    #[test]
    fn a_stall_the_ring_covers_costs_no_underrun_and_shows_in_the_mark() {
        let mut rig = Rig::primed();
        for _ in 0..20 {
            rig.period(true);
        }
        // The fill priming left, then the steady draw of one half: loaded,
        // then refetched within the period.
        assert_eq!(rig.stream.take_ring_min(), SLOTS - 2);
        for _ in 0..20 {
            rig.period(true);
        }
        assert_eq!(rig.stream.take_ring_min(), SLOTS - 1);

        // The relay holds one fetch for SLOTS - 3 half periods. Each
        // period loads one half from the ring and nothing comes back, and
        // the first period after it loads one more before the held fetch
        // lands: the ring is drawn down to 2.
        for _ in 0..SLOTS - 3 {
            rig.period(false);
        }
        for _ in 0..100 {
            rig.period(true);
        }
        let line = rig.halves();
        assert_eq!(line.underruns, 0);
        assert_eq!(rig.pl.loaded, in_order(line.halves));
        assert!(rig.pl.played.iter().all(Option::is_some));
        assert_eq!(rig.stream.take_ring_min(), 2);

        // Refilled: back to the steady draw of one.
        for _ in 0..20 {
            rig.period(true);
        }
        assert_eq!(rig.stream.take_ring_min(), SLOTS - 1);
    }

    #[test]
    fn a_stall_longer_than_the_ring_underruns_and_recovers_in_order() {
        let mut rig = Rig::primed();
        for _ in 0..20 {
            rig.period(true);
        }
        // The ring lasts SLOTS half periods; each one past that the PL
        // flips into a half nobody loaded.
        for _ in 0..SLOTS + 5 {
            rig.period(false);
        }
        assert_eq!(rig.stream.take_ring_min(), 0);
        // The last flip's underrun is seen by the next service.
        rig.period(true);
        let underruns = rig.halves().underruns;
        assert_eq!(underruns, 5);
        assert_eq!(rig.pl.played.iter().filter(|p| p.is_none()).count(), 5);

        for _ in 0..100 {
            rig.period(true);
        }
        let line = rig.halves();
        // Over once the fetches resume.
        assert_eq!(line.underruns, underruns);
        // Loaded in order with none skipped or repeated...
        assert_eq!(rig.pl.loaded, in_order(line.halves));
        // ...and played that way, the stale halves aside.
        let fresh: Vec<u32> = rig.pl.played.iter().flatten().copied().collect();
        assert_eq!(fresh, in_order(len_u32(&fresh)));
        assert!(rig.pl.played.ends_with(&[Some((line.halves - 1) * STEP)]));
    }

    #[test]
    fn a_failed_fetch_is_retried_at_the_same_offset_into_the_same_slot() {
        let mut rig = Rig::primed();
        rig.service();
        assert_eq!(rig.stream.in_flight(), Some(0));
        assert_eq!(rig.fetch.offsets.last(), Some(&(16 * 147)));

        rig.fetch.state = FetchState::Failed;
        rig.service();
        assert_eq!(rig.stream.in_flight(), None);
        assert_eq!(rig.halves().failures, 1);
        assert_eq!(rig.halves().next, 16 * 147);

        rig.service();
        assert_eq!(rig.fetch.offsets.last(), Some(&(16 * 147)));
        assert_eq!(rig.stream.in_flight(), Some(0));

        // The retry lands, and the PL still gets every offset in order.
        for _ in 0..40 {
            rig.period(true);
        }
        assert_eq!(rig.halves().underruns, 0);
        assert_eq!(rig.pl.loaded, in_order(rig.halves().halves));
    }

    #[test]
    fn a_failed_prime_is_retried_at_the_same_offset_into_the_same_slot() {
        let mut rig = Rig::new();
        rig.begin();
        rig.complete();
        rig.fetch.state = FetchState::Failed;
        rig.service();
        assert_eq!(rig.stream.in_flight(), None);
        rig.service();
        assert_eq!(rig.fetch.offsets, [0, 147, 147]);
        assert_eq!(rig.stream.in_flight(), Some(1));
        assert_eq!(rig.halves().failures, 1);

        for _ in 1..SLOTS {
            rig.complete();
        }
        assert!(rig.stream.is_running());
        assert_eq!(rig.pl.loaded, [0, 147]);
    }

    #[test]
    fn a_request_that_does_not_leave_is_a_failure_retried_next_time() {
        let mut rig = Rig::new();
        rig.fetch.refuse = true;
        rig.begin();
        assert_eq!(rig.stream.in_flight(), None);
        assert_eq!(rig.halves().failures, 1);

        rig.fetch.refuse = false;
        rig.service();
        assert_eq!(rig.fetch.offsets, [0]);
        assert_eq!(rig.stream.in_flight(), Some(0));
    }

    #[test]
    fn a_refused_request_while_priming_is_retried_at_its_offset() {
        let mut rig = Rig::new();
        rig.begin();
        rig.fetch.refuse = true;
        rig.complete();
        assert_eq!(rig.stream.in_flight(), None);

        rig.fetch.refuse = false;
        rig.service();
        assert_eq!(rig.fetch.offsets, [0, 147]);
        assert_eq!(rig.stream.in_flight(), Some(1));
    }

    #[test]
    fn an_underrun_is_counted_and_acked_every_time_it_shows() {
        let mut rig = Rig::primed();
        rig.pl.status = UNDERRUN;
        rig.service();
        assert_eq!(rig.pl.ops, [Op::Ack(replay_ack::UNDERRUN)]);
        assert_eq!(rig.pl.status, 0);
        rig.service();
        rig.pl.ops.clear();
        rig.pl.status = UNDERRUN | CONSUMED0 | PLAY_HALF;
        rig.service();
        assert_eq!(rig.halves().underruns, 2);
        // Acked first, and the load still happens in the same call.
        assert_eq!(
            rig.pl.ops,
            [
                Op::Ack(replay_ack::UNDERRUN),
                Op::Load(Half::Zero, 2),
                Op::Ack(replay_ack::CONSUMED0),
            ]
        );
    }

    #[test]
    fn begin_resets_the_counts_and_the_ring() {
        let mut rig = Rig::primed();
        rig.pl.status = UNDERRUN;
        rig.service();
        rig.begin();
        let line = rig.halves();
        assert_eq!((line.halves, line.underruns, line.failures), (0, 0, 0));
        assert_eq!(rig.stream.in_flight(), Some(0));
        assert_eq!(rig.fetch.offsets.last(), Some(&0));
    }

    #[test]
    fn fetch_flush_and_gap_are_timed_and_reset_by_the_report() {
        let mut rig = Rig::new();
        rig.begin();
        for _ in 0..SLOTS {
            rig.clock.advance(800);
            rig.complete();
        }
        // Running: the first fetch is requested 3000 us later.
        rig.clock.advance(3000);
        rig.pl.flip();
        rig.service();
        rig.clock.advance(500);
        rig.complete();

        let t = rig.stream.take_timings();
        // The sixteen priming fetches and the one just completed.
        assert_eq!((t.fetch.n(), t.fetch.max(), t.fetch.avg()), (17, 800, 782));
        // The fake load takes no time: the two priming halves and one more.
        assert_eq!((t.flush.n(), t.flush.max()), (3, 0));
        // Priming is not a gap, and the fetch after the last one completed
        // left at once.
        assert_eq!((t.gap.n(), t.gap.max()), (1, 0));
        assert_eq!(rig.stream.take_timings(), StreamTimings::default());
    }

    #[test]
    fn the_report_carries_the_c_fields_and_the_ring_min() {
        let mut rig = Rig::primed();
        let mut rx = RxStats::new();
        rx.wakeup(3);
        rx.wakeup(0);
        rx.datagram(4, 20);
        rx.datagram(6, 30);
        rx.datagram(5, 25);
        let inputs = ReportInputs {
            status: ReplayStatus(0x0012_3405),
            client: argus_replay::Counters {
                requests_sent: 3,
                retransmits_sent: 1,
                chunks_accepted: 63,
                chunks_rejected: 2,
                timeouts: 1,
            },
            con_drops: 7,
            con_max: 180,
        };
        let report = StreamReport::take(&mut rig.stream, &mut rx, &inputs);
        assert_eq!(
            format!("{}", report.halves),
            "stream: halves=2 underruns=0 failures=0 next=2352 pl: half=1 row=4660 c0=0 c1=1\r\n"
        );
        assert_eq!(
            format!("{}", report.fetch),
            "stream: fetch avg=0 max=0 us (n=16)  flush avg=0 max=0 us  rtx=1 to=1 rej=2  \
             ring min=14/16\r\n"
        );
        assert_eq!(
            format!("{}", report.rx),
            "stream: rx n=3 copy avg=5 max=6 us  parse avg=25 max=30 us  \
             loop iters=2 pkts=3  gap avg=0 max=0 us (n=0)  con drop=7 max=180\r\n"
        );
        assert_eq!(rx, RxStats::new());
    }
}
