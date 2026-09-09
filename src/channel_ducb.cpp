#include "channel_ducb.h"
#include <math.h>

// 2.4 GHz channels — 1..13 (US-legal 1..11 still work; the extra 12/13 arms
// just stay under-explored where illegal, which is the same effect as a
// tighter reward. Keeping the whole EU table means no config knob needed.)
#define DUCB_N        13
#define DUCB_GAMMA    0.99   // decay factor per selection — ~100-hop memory
#define DUCB_C        1.0    // exploration constant (matches projectZero)

struct Arm {
    double reward;   // discounted sum of rewards
    double pulls;    // discounted number of times selected
};

static Arm    s_arms[DUCB_N];
static double s_total = 0.0;   // Σ discounted pulls across all arms

void channel_ducb_reset()
{
    for (int i = 0; i < DUCB_N; i++) { s_arms[i].reward = 0; s_arms[i].pulls = 0; }
    s_total = 0.0;
}

int channel_ducb_select()
{
    // Discount everything BEFORE choosing — this is the "D" in D-UCB. It
    // gently forgets old observations so we adapt when the environment
    // shifts (e.g. someone parks a new AP on channel 6).
    s_total *= DUCB_GAMMA;
    for (int i = 0; i < DUCB_N; i++) {
        s_arms[i].reward *= DUCB_GAMMA;
        s_arms[i].pulls  *= DUCB_GAMMA;
    }

    // Forced exploration: any arm we've barely visited has an "infinite"
    // UCB, so gets picked first. This is what makes the first ~13 hops
    // behave like round-robin (each channel visited once) before we start
    // exploiting.
    for (int i = 0; i < DUCB_N; i++)
        if (s_arms[i].pulls < 1e-3) {
            return i + 1;   // channels are 1-indexed
        }

    int    best_idx = 0;
    double best_ucb = -1e30;
    // log(0) is undefined, so nudge the argument by +1 — same trick as the
    // reference implementation. With s_total ≥ 13 after the cold start
    // this is a non-issue in practice.
    double log_total = log(s_total + 1.0);
    for (int i = 0; i < DUCB_N; i++) {
        double mean        = s_arms[i].reward / s_arms[i].pulls;
        double exploration = DUCB_C * sqrt(log_total / s_arms[i].pulls);
        double ucb         = mean + exploration;
        if (ucb > best_ucb) { best_ucb = ucb; best_idx = i; }
    }
    return best_idx + 1;
}

void channel_ducb_reward(int channel, double reward)
{
    if (channel < 1 || channel > DUCB_N) return;
    int i = channel - 1;
    // The pull is credited on select(), not on reward — so reward-only
    // callers just top up the arm's numerator. In our wiring select() is
    // called by the hop timer, so the pull always precedes any rewards
    // credited during that hop's dwell.
    s_arms[i].reward += reward;
}

double channel_ducb_avg(int channel)
{
    if (channel < 1 || channel > DUCB_N) return -1.0;
    int i = channel - 1;
    if (s_arms[i].pulls < 1e-3) return -1.0;
    return s_arms[i].reward / s_arms[i].pulls;
}

void channel_ducb_pull(int channel)
{
    if (channel < 1 || channel > DUCB_N) return;
    s_arms[channel - 1].pulls += 1.0;
    s_total += 1.0;
}
