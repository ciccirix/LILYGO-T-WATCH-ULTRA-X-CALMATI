#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Discounted UCB (D-UCB) channel picker for 2.4 GHz WiFi channel hopping —
// same technique used by C5Lab/projectZero (MIT) for its dual-band scanner.
//
// The bandit models each channel as an arm whose reward is the number of
// interesting frames seen while it was selected (beacons for the wardriver,
// EAPOL for handshake capture, etc.). It picks the channel maximising:
//
//     mean_reward_discounted + c * sqrt(ln(total_pulls_discounted) / n_i)
//
// where every pull and reward is multiplied by γ before each selection so
// stale observations decay. That's the trick that beats plain UCB in a
// non-stationary environment (people move, APs change channel, one channel
// suddenly gets loud) — with γ=0.99 the effective horizon is ~100 hops.
//
// The exploration term guarantees any never-visited channel gets picked first
// (its confidence bound is infinite), so cold-start behaves like round-robin
// for the first ~13 hops and then hones in on productive channels.
//
// State is tiny (13 doubles × 3 = ~ 300 B), so no PSRAM allocation.
// Thread-model: single writer (the LVGL/main task drives the hop timer AND
// the promiscuous callback also runs on main — dispatch happens there); no
// locking.
// ---------------------------------------------------------------------------

// Reset all arms. Call once when starting the wardriver / scanner.
void channel_ducb_reset();

// Pick the next 2.4 GHz channel (1..13). Applies the γ discount, then
// argmaxes over UCB.
int channel_ducb_select();

// Feed +reward for `channel` (typically 1.0 for one interesting frame).
// Safe to call from the promiscuous callback if it runs on the same task
// as the hop timer.
void channel_ducb_reward(int channel, double reward);

// Register that we've physically moved the radio onto `channel` — bumps the
// pull counter for that arm. Call once at start (for the initial channel)
// and once per channel-hop after the radio actually changes.
void channel_ducb_pull(int channel);

// Debug helper: recent hit rate on `channel` (0..1 range, discounted), or
// -1 if the channel has never been pulled. Not used by the UI yet, but
// handy from the serial console.
double channel_ducb_avg(int channel);
