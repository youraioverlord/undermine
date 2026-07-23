#include "synth.h"
#include <math.h>
#include <string.h>

enum { WAVE_PULSE = 0, WAVE_TRI, WAVE_SAW, WAVE_NOISE };

/* Effect table. Frequencies in Hz, durations in ms. Kept short and punchy —
 * jaunty for pickups, descending/noisy for death, per DESIGN.md §4.3. */
static const SynthSfx SFX[SFX_COUNT] = {
    /* SFX_JUMP    */ { { {330, WAVE_PULSE, 28, 0.22f}, {494, WAVE_PULSE, 36, 0.22f} }, 2, 1 },
    /* SFX_LAND    */ { { {120, WAVE_NOISE, 36, 0.16f} }, 1, 1 },
    /* SFX_COAL    */ { { {880, WAVE_TRI, 45, 0.28f}, {1175, WAVE_TRI, 55, 0.28f} }, 2, 2 },
    /* SFX_QUOTA   */ { { {660, WAVE_TRI, 60, 0.30f}, {880, WAVE_TRI, 60, 0.30f},
                          {1320, WAVE_TRI, 100, 0.30f} }, 3, 2 },
    /* SFX_DEATH   */ { { {440, WAVE_PULSE, 80, 0.30f}, {330, WAVE_PULSE, 80, 0.30f},
                          {220, WAVE_PULSE, 90, 0.30f}, {150, WAVE_NOISE, 150, 0.30f} }, 4, 3 },
    /* SFX_DESCEND */ { { {523, WAVE_TRI, 50, 0.26f}, {392, WAVE_TRI, 50, 0.26f},
                          {262, WAVE_TRI, 80, 0.26f} }, 3, 1 },
    /* SFX_SLAM    */ { { {100, WAVE_NOISE, 40, 0.32f}, {80, WAVE_NOISE, 90, 0.14f} }, 2, 1 },
};

/* Song patterns, retriggered per row for a plucky, SID-like feel. 0 = rest.
 * The arp row is chord ROOTS strummed as fast triads on a third voice
 * (negative root = minor chord). In-game runs melody + bass only and
 * deliberately leaves gaps so SFX breathe.
 *
 * Title: 32 rows, question phrase then an answering one, over C / Am / G / F.
 * Game over (§4.2): the title's opening line pulled into A minor, walking
 * down to an E-major turn and settling on the Am it should have avoided. */
static const int TITLE_MEL[32]  = {523,0,659,0,784,0,659,0, 440,0,523,0,392,0,  0,0,
                                   523,0,659,0,880,0,784,0, 659,0,587,0,523,0,  0,0};
static const int TITLE_BASS[32] = {131,0, 98,0,131,0, 98,0, 110,0, 82,0, 98,0, 98,0,
                                   131,0, 98,0,175,0,131,0, 110,0,123,0,131,0, 98,0};
static const int TITLE_ARP[32]  = {262,0,0,0, 262,0,0,0, -220,0,0,0, 196,0,0,0,
                                   262,0,0,0, 349,0,0,0, -220,0,0,0, 262,0,0,0};
static const int GAME_MEL[16]   = {0,0,0,0,440,0,0,0,0,0,0,0,392,0,0,0};
static const int GAME_BASS[16]  = {131,0,0,131,0,0,98,0,131,0,0,131,0,0,110,0};
static const int OVER_MEL[16]   = {659,0,587,0, 523,0,494,0, 440,0,349,0, 330,0,440,0};
static const int OVER_BASS[16]  = {110,0,110,0,  98,0, 98,0,  87,0, 87,0,  82,0,110,0};
static const int OVER_ARP[16]   = {-220,0,0,0, 196,0,0,0, 175,0,0,0, 165,0,-220,0};

typedef struct {
    const int *mel, *bass, *arp;
    int len, rowMs, melWave, bassWave;
    bool loop;
} Song;

static const Song SONGS[SONG_COUNT] = {
    /* SONG_TITLE  */ {TITLE_MEL, TITLE_BASS, TITLE_ARP, 32, 125, WAVE_PULSE, WAVE_TRI,   true},
    /* SONG_INGAME */ {GAME_MEL,  GAME_BASS,  NULL,      16, 165, WAVE_TRI,   WAVE_PULSE, true},
    /* SONG_OVER   */ {OVER_MEL,  OVER_BASS,  OVER_ARP,  16, 165, WAVE_TRI,   WAVE_TRI,   false},
};

void synth_init(Synth *s)
{
    int i;
    memset(s, 0, sizeof *s);
    s->rate = SYNTH_RATE;
    for (i = 0; i < SYNTH_VOICES; i++) s->v[i].noise = 0x7FFFu;
    s->music.pitch = 1.0f;
}

void synth_trigger(Synth *s, SfxId id)
{
    const SynthSfx *sfx = &SFX[id];
    int i, slot = -1, lo;

    for (i = 0; i < SYNTH_VOICES; i++)          /* prefer an idle voice */
        if (!s->v[i].active) { slot = i; break; }
    if (slot < 0) {                             /* else steal lowest priority */
        lo = 0;
        for (i = 1; i < SYNTH_VOICES; i++)
            if (s->v[i].priority < s->v[lo].priority) lo = i;
        if (sfx->priority >= s->v[lo].priority) slot = lo;
        else return;                            /* don't interrupt something louder */
    }
    s->v[slot].active = true;
    s->v[slot].sfx = sfx;
    s->v[slot].segIdx = 0;
    s->v[slot].sampleInSeg = 0;
    s->v[slot].phase = 0.0f;
    s->v[slot].priority = sfx->priority;
    if (id == SFX_DEATH)   /* duck the music under the jingle, fade it back */
        s->music.duckPos = s->music.duckTotal = s->rate * 4 / 5;
}

void synth_music_play(Synth *s, SongId id)
{
    const Song *song = &SONGS[id];
    s->music.mel = song->mel;
    s->music.bass = song->bass;
    s->music.arp = song->arp;
    s->music.len = song->len;
    s->music.rowSamples = song->rowMs * s->rate / 1000;
    s->music.melWave = song->melWave;
    s->music.bassWave = song->bassWave;
    s->music.loop = song->loop;
    s->music.row = 0;
    s->music.sampleInRow = 0;
    s->music.arpStep = 0;
    s->music.arpStepSamp = 0;
    s->music.duckPos = 0;
    s->music.sweepPos = 0;
    s->music.lp = 0.0f;
    s->music.on = true;
}

void synth_music_stop(Synth *s) { s->music.on = false; }
void synth_set_pitch(Synth *s, float mult) { s->music.pitch = mult; }
void synth_sweep(Synth *s) { s->music.sweepPos = s->music.sweepTotal = s->rate; }

static float gen_wave(float *phase, float freq, int wave, int rate, unsigned int *noise)
{
    float s = 0.0f;
    switch (wave) {
    case WAVE_PULSE: s = (*phase < 0.5f) ? 0.6f : -0.6f; break;
    case WAVE_TRI:   s = 2.0f * fabsf(2.0f * *phase - 1.0f) - 1.0f; break;
    case WAVE_SAW:   s = 2.0f * *phase - 1.0f; break;
    case WAVE_NOISE:
        *noise = (*noise >> 1) ^ ((unsigned)(-(int)(*noise & 1u)) & 0xB400u);
        s = (*noise & 1u) ? 0.5f : -0.5f;
        break;
    }
    *phase += freq / (float)rate;
    if (*phase >= 1.0f) *phase -= 1.0f;
    return s;
}

static float voice_sample(SynthVoice *v, int rate)
{
    const SynthSeg *seg;
    int segSamples, atk, rel;
    float env = 1.0f, out;

    if (!v->active) return 0.0f;
    seg = &v->sfx->seg[v->segIdx];
    segSamples = seg->ms * rate / 1000;
    if (segSamples < 1) segSamples = 1;
    atk = rate * 2 / 1000;          /* 2ms attack  */
    rel = rate * 4 / 1000;          /* 4ms release — kills clicks */
    if (v->sampleInSeg < atk)
        env = (float)v->sampleInSeg / (float)atk;
    else if (v->sampleInSeg > segSamples - rel)
        env = (float)(segSamples - v->sampleInSeg) / (float)rel;
    if (env < 0.0f) env = 0.0f;

    out = gen_wave(&v->phase, seg->freq, seg->wave, rate, &v->noise) * seg->vol * env;

    if (++v->sampleInSeg >= segSamples) {       /* advance the score */
        v->sampleInSeg = 0;
        v->phase = 0.0f;
        if (++v->segIdx >= v->sfx->n) { v->active = false; v->priority = 0; }
    }
    return out;
}

static float music_sample(SynthMusic *m, int rate)
{
    int mf, bf, af, atk;
    float env, out = 0.0f;
    unsigned int dummy = 0;

    if (!m->on || m->rowSamples < 1) return 0.0f;
    mf = m->mel[m->row];
    bf = m->bass[m->row];
    af = m->arp ? m->arp[m->row] : 0;
    env = 1.0f - (float)m->sampleInRow / (float)m->rowSamples;   /* per-row pluck decay */
    if (env < 0.0f) env = 0.0f;
    atk = rate * 3 / 1000;
    if (m->sampleInRow < atk) env *= (float)m->sampleInRow / (float)atk;

    if (mf > 0) out += gen_wave(&m->melPhase,  mf * m->pitch, m->melWave,  rate, &dummy) * 0.11f * env;
    if (bf > 0) out += gen_wave(&m->bassPhase, bf * m->pitch, m->bassWave, rate, &dummy) * 0.14f * env;
    if (af != 0) {
        /* the SID trick: one voice strums a triad by hopping between chord
         * tones at ~50 Hz — root, third (minor when the root is negative),
         * fifth. Reads as a chord, costs one voice. */
        static const float MAJ[3] = {1.0f, 1.25f, 1.5f};
        static const float MIN[3] = {1.0f, 1.2f,  1.5f};
        float root = (float)(af < 0 ? -af : af);
        float mul  = (af < 0 ? MIN : MAJ)[m->arpStep];
        out += gen_wave(&m->arpPhase, root * mul * m->pitch, WAVE_SAW, rate, &dummy) * 0.07f * env;
    }
    if (++m->arpStepSamp >= rate / 50) { m->arpStepSamp = 0; m->arpStep = (m->arpStep + 1) % 3; }

    if (m->duckPos > 0) {   /* death jingle owns the stage; music fades back in */
        out *= 0.25f + 0.75f * (1.0f - (float)m->duckPos / (float)m->duckTotal);
        m->duckPos--;
    }
    if (m->sweepPos > 0) {  /* descending dread: low-pass dips to ~250 Hz and back */
        float p  = 1.0f - (float)m->sweepPos / (float)m->sweepTotal;
        float tri = 1.0f - fabsf(2.0f * p - 1.0f);
        float fc = 3200.0f - 2950.0f * tri;
        float a  = 1.0f - expf(-6.2831853f * fc / (float)rate);
        m->lp += a * (out - m->lp);
        out = m->lp;
        m->sweepPos--;
    }

    if (++m->sampleInRow >= m->rowSamples) {
        m->sampleInRow = 0;
        if (++m->row >= m->len) {
            m->row = 0;
            if (!m->loop) m->on = false;   /* a jingle says its piece and stops */
        }
    }
    return out;
}

void synth_render(Synth *s, float *out, int frames)
{
    int i, k;
    for (i = 0; i < frames; i++) {
        float mix = music_sample(&s->music, s->rate);
        for (k = 0; k < SYNTH_VOICES; k++) mix += voice_sample(&s->v[k], s->rate);
        if (mix > 1.0f) mix = 1.0f;
        if (mix < -1.0f) mix = -1.0f;
        out[i] = mix;
    }
}
