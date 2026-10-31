//! The bookkeeping of the read-ahead ring: which slots hold fetched halves
//! and which one the PL gets next. No storage; the firmware keeps the
//! halves themselves, [`SLOTS`] of them, and indexes them with the slot
//! numbers this hands out.
//!
//! The replay stream fetches ahead into the ring and loads each half the PL
//! frees from it, so a relay that answers late draws the ring down instead
//! of costing an underrun. Fetches fill slots at one end, in order; the PL
//! is fed from the other end, oldest first. That order is the dataset's
//! order, which the bit-exact comparison depends on: a slot is never
//! skipped and never handed out twice.
//!
//! A misused ring (a commit when it is full, a release when it is empty)
//! changes nothing and says so in the return value. Nothing here panics.

/// The firmware's ring size, in halves. Sixteen halves of 4.9 ms each
/// cover about 75 ms of a relay that does not answer.
pub const SLOTS: usize = 16;

/// Which of `N` slots are filled, as a run of `len` slots from the oldest,
/// wrapping at `N`.
///
/// The fetch side asks [`next_free`](Self::next_free) where the next fetch
/// lands and [`commit`](Self::commit)s it once it is complete; the play
/// side asks for the [`oldest`](Self::oldest) filled slot and
/// [`release`](Self::release)s it once it is loaded into the PL.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ring<const N: usize> {
    /// The oldest filled slot, or where the next fill lands when empty.
    /// Always below `N`.
    oldest: usize,
    /// Filled slots, 0 to `N`.
    len: usize,
    /// The fewest filled slots since the last
    /// [`take_low_water`](Self::take_low_water).
    low: usize,
}

impl<const N: usize> Default for Ring<N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize> Ring<N> {
    /// A ring of no slots would make every slot number meaningless (and the
    /// wrap a division by zero), so it does not compile.
    const NOT_EMPTY: () = assert!(N > 0, "a ring needs at least one slot");

    /// An empty ring. `const`, so the firmware can keep one in a static.
    pub const fn new() -> Self {
        // Evaluating the constant is what makes `Ring<0>` fail to build.
        let () = Self::NOT_EMPTY;
        Self {
            oldest: 0,
            len: 0,
            low: 0,
        }
    }

    /// How many slots the ring has.
    pub const fn capacity(&self) -> usize {
        N
    }

    /// Filled slots: fetched and not yet loaded into the PL.
    pub const fn len(&self) -> usize {
        self.len
    }

    /// Whether no slot is filled.
    pub const fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Whether every slot is filled.
    pub const fn is_full(&self) -> bool {
        self.len == N
    }

    /// The slot the next fetch lands in, or `None` when the ring is full.
    pub const fn next_free(&self) -> Option<usize> {
        if self.is_full() {
            None
        } else {
            Some(Self::wrap(self.oldest + self.len))
        }
    }

    /// Marks the slot [`next_free`](Self::next_free) named as filled: a
    /// fetch into it completed, and it becomes the newest. Returns `false`,
    /// changing nothing, when the ring is full.
    pub fn commit(&mut self) -> bool {
        if self.is_full() {
            return false;
        }
        self.len += 1;
        true
    }

    /// The oldest filled slot, the next to load into the PL, or `None` when
    /// the ring is empty.
    pub const fn oldest(&self) -> Option<usize> {
        if self.is_empty() {
            None
        } else {
            Some(self.oldest)
        }
    }

    /// Frees the slot [`oldest`](Self::oldest) named, once it is loaded into
    /// the PL. Returns `false`, changing nothing, when the ring is empty.
    pub fn release(&mut self) -> bool {
        if self.is_empty() {
            return false;
        }
        self.oldest = Self::wrap(self.oldest + 1);
        self.len -= 1;
        // Only a release lowers the fill, so this is the only place the
        // low-water mark can fall.
        self.low = self.low.min(self.len);
        true
    }

    /// The fewest filled slots seen since the last call (or since the ring
    /// was made), for the stream's report; starts again from the current
    /// fill.
    pub fn take_low_water(&mut self) -> usize {
        core::mem::replace(&mut self.low, self.len)
    }

    /// Brings `slot`, below `2 * N`, back into the ring. A subtraction
    /// rather than `%`: both callers add at most `N` to a slot below `N`.
    const fn wrap(slot: usize) -> usize {
        if slot >= N { slot - N } else { slot }
    }
}
