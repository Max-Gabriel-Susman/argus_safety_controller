//! The read-ahead ring's bookkeeping: slots handed to the PL in the order
//! they were filled across many wraps, the full and empty edges, misuse
//! that changes nothing, and the low-water mark the stream reports.

use argus_fw_core::ring::{Ring, SLOTS};

/// Fills the next free slot, as a completed fetch would, and returns it.
fn fill<const N: usize>(ring: &mut Ring<N>) -> usize {
    let slot = ring.next_free();
    assert!(ring.commit());
    slot.unwrap_or(usize::MAX)
}

/// Loads and frees the oldest slot, as the play side would, and returns it.
fn drain_one<const N: usize>(ring: &mut Ring<N>) -> usize {
    let slot = ring.oldest();
    assert!(ring.release());
    slot.unwrap_or(usize::MAX)
}

#[test]
fn the_firmware_ring_has_sixteen_slots() {
    let ring = Ring::<SLOTS>::new();
    assert_eq!(SLOTS, 16);
    assert_eq!(ring.capacity(), SLOTS);
    assert!(ring.is_empty());
    assert_eq!(ring.len(), 0);
    assert_eq!(ring.next_free(), Some(0));
    assert_eq!(ring.oldest(), None);
}

#[test]
fn slots_come_out_in_the_order_they_went_in_across_wraps() {
    let mut ring = Ring::<4>::new();
    // The dataset offset each slot holds, as the firmware's storage would.
    let mut held = [None; 4];
    let mut fetched = 0u32;
    let mut loaded = Vec::new();

    // Uneven bursts on each side, so the two ends meet the wrap at
    // different points: 1 to 4 fills, then 1 to 3 loads, many times over.
    for round in 0..50usize {
        for _ in 0..=(round % 4) {
            let Some(slot) = ring.next_free() else { break };
            // A slot out of range would leave its offset unrecorded, and
            // the comparison below would catch the gap.
            if let Some(h) = held.get_mut(slot) {
                *h = Some(fetched);
            }
            fetched += 1;
            assert!(ring.commit());
        }
        for _ in 0..=(round % 3) {
            let Some(slot) = ring.oldest() else { break };
            loaded.push(held.get_mut(slot).and_then(Option::take));
            assert!(ring.release());
        }
    }
    while let Some(slot) = ring.oldest() {
        loaded.push(held.get_mut(slot).and_then(Option::take));
        assert!(ring.release());
    }

    // Every fetch loaded once, in order, and many more than four of them,
    // so the slots wrapped many times.
    let expected: Vec<_> = (0..fetched).map(Some).collect();
    assert_eq!(loaded, expected);
    assert!(fetched > 40);
}

#[test]
fn slot_numbers_walk_round_the_ring() {
    let mut ring = Ring::<3>::new();
    let mut filled = Vec::new();
    let mut drained = Vec::new();
    for _ in 0..7 {
        filled.push(fill(&mut ring));
        filled.push(fill(&mut ring));
        drained.push(drain_one(&mut ring));
        drained.push(drain_one(&mut ring));
    }
    assert_eq!(filled, [0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1]);
    assert_eq!(drained, filled);
}

#[test]
fn a_full_ring_offers_no_slot_to_fetch_into() {
    let mut ring = Ring::<4>::new();
    for expected in 0..4 {
        assert!(!ring.is_full());
        assert_eq!(ring.next_free(), Some(expected));
        assert!(ring.commit());
    }
    assert!(ring.is_full());
    assert_eq!(ring.len(), 4);
    assert_eq!(ring.next_free(), None);
    assert_eq!(ring.oldest(), Some(0));

    // Freeing one makes exactly that slot the next to fill.
    assert!(ring.release());
    assert_eq!(ring.next_free(), Some(0));
    assert_eq!(ring.oldest(), Some(1));
}

#[test]
fn an_empty_ring_offers_nothing_to_load() {
    let mut ring = Ring::<4>::new();
    assert_eq!(ring.oldest(), None);

    // Filled and drained part way round, it is empty again with the next
    // fill landing where the last one left off.
    fill(&mut ring);
    fill(&mut ring);
    fill(&mut ring);
    drain_one(&mut ring);
    drain_one(&mut ring);
    drain_one(&mut ring);
    assert!(ring.is_empty());
    assert_eq!(ring.oldest(), None);
    assert_eq!(ring.next_free(), Some(3));
}

#[test]
fn a_one_slot_ring_alternates_full_and_empty() {
    let mut ring = Ring::<1>::new();
    for _ in 0..3 {
        assert_eq!(ring.next_free(), Some(0));
        assert!(ring.commit());
        assert!(ring.is_full());
        assert_eq!(ring.next_free(), None);
        assert_eq!(ring.oldest(), Some(0));
        assert!(ring.release());
        assert!(ring.is_empty());
    }
}

#[test]
fn committing_into_a_full_ring_changes_nothing() {
    let mut ring = Ring::<4>::new();
    fill(&mut ring);
    drain_one(&mut ring);
    for _ in 0..4 {
        fill(&mut ring);
    }
    let before = ring;
    assert!(!ring.commit());
    assert!(!ring.commit());
    assert_eq!(ring, before);
    assert_eq!(ring.len(), 4);
    assert_eq!(ring.oldest(), Some(1));
}

#[test]
fn releasing_from_an_empty_ring_changes_nothing() {
    let mut ring = Ring::<4>::new();
    fill(&mut ring);
    fill(&mut ring);
    drain_one(&mut ring);
    drain_one(&mut ring);
    ring.take_low_water();
    let before = ring;
    assert!(!ring.release());
    assert!(!ring.release());
    assert_eq!(ring, before);
    assert_eq!(ring.next_free(), Some(2));
    // Nor did the refused release count as a lower fill.
    assert_eq!(ring.take_low_water(), 0);
}

#[test]
fn the_low_water_mark_is_the_fewest_filled_since_the_last_take() {
    let mut ring = Ring::<SLOTS>::new();
    // Made empty, so the first take says so.
    for _ in 0..SLOTS {
        fill(&mut ring);
    }
    assert_eq!(ring.take_low_water(), 0);

    // Drawn down to 11, then refilled: the take reports 11, not the
    // current 16.
    for _ in 0..5 {
        drain_one(&mut ring);
    }
    for _ in 0..5 {
        fill(&mut ring);
    }
    assert!(ring.is_full());
    assert_eq!(ring.take_low_water(), 11);

    // The next report starts from the fill at the take, 16, and sees only
    // what happens after it: one load and one fill.
    drain_one(&mut ring);
    fill(&mut ring);
    assert_eq!(ring.take_low_water(), 15);

    // Two dips in one report: the deeper one counts.
    for _ in 0..3 {
        drain_one(&mut ring);
    }
    for _ in 0..3 {
        fill(&mut ring);
    }
    for _ in 0..7 {
        drain_one(&mut ring);
    }
    for _ in 0..2 {
        fill(&mut ring);
    }
    assert_eq!(ring.len(), 11);
    assert_eq!(ring.take_low_water(), 9);
}

#[test]
fn a_take_with_nothing_loaded_since_reports_the_current_fill() {
    let mut ring = Ring::<SLOTS>::new();
    for _ in 0..10 {
        fill(&mut ring);
    }
    for _ in 0..4 {
        drain_one(&mut ring);
    }
    assert_eq!(ring.take_low_water(), 0);

    // Nothing loaded: the fill stayed at 6, so 6 is the low.
    assert_eq!(ring.take_low_water(), 6);
    // Fills alone only raise the fill, so the low is still where the
    // report started.
    fill(&mut ring);
    fill(&mut ring);
    assert_eq!(ring.take_low_water(), 6);
    assert_eq!(ring.take_low_water(), 8);
}
