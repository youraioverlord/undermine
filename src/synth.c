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
};

/* Two 16-row looping songs (melody + bass), retriggered per row for a plucky,
 * SID-like feel. 0 = rest. In-game deliberately leaves gaps so SFX breathe. */
static const int TITLE_MEL[16]  = {523,0,659,0,784,0,659,0,440,0,523,0,392,0,0,0};
static const int TITLE_BASS[16] = {131,0, 98,0,131,0, 98,0,110,0, 82,0, 98,0,98,0};
static const int GAME_MEL[16]   = {0,0,0,0,440,0,0,0,0,0,0,0,392,0,0,0};
static const int GAME_BASS[16]  = {131,0,0,131,0,0,98,0,131,0,0,131,0,0,110,0};

typedef struct {
    const int *mel, *bass;
    int len, rowMs, melWave, bassWave;
} Song;

static const Song SONGS[SONG_COUNT] = {
    /* SONG_TITLE  */ {TITLE_MEL, TITLE_BASS, 16, 125, WAVE_PULSE, WAVE_TRI},
    /* SONG_INGAME */ {GAME_MEL,  GAME_BASS,  16, 165, WAVE_TRI,   WAVE_PULSE},
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
}

void synth_music_play(Synth *s, SongId id)
{
    const Song *song = &SONGS[id];
    s->music.mel = song->mel;
    s->music.bass = song->bass;
    s->music.len = song->len;
    s->music.rowSamples = song->rowMs * s->rate / 1000;
    s->music.melWave = song->melWave;
    s->music.bassWave = song->bassWave;
    s->music.row = 0;
    s->music.sampleInRow = 0;
    s->music.on = true;
}

void synth_music_stop(Synth *s) { s->music.on = false; }
void synth_set_pitch(Synth *s, float mult) { s->music.pitch = mult; }

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
    int mf, bf, atk;
    float env, out = 0.0f;
    unsigned int dummy = 0;

    if (!m->on || m->rowSamples < 1) return 0.0f;
    mf = m->mel[m->row];
    bf = m->bass[m->row];
    env = 1.0f - (float)m->sampleInRow / (float)m->rowSamples;   /* per-row pluck decay */
    if (env < 0.0f) env = 0.0f;
    atk = rate * 3 / 1000;
    if (m->sampleInRow < atk) env *= (float)m->sampleInRow / (float)atk;

    if (mf > 0) out += gen_wave(&m->melPhase,  mf * m->pitch, m->melWave,  rate, &dummy) * 0.11f * env;
    if (bf > 0) out += gen_wave(&m->bassPhase, bf * m->pitch, m->bassWave, rate, &dummy) * 0.14f * env;

    if (++m->sampleInRow >= m->rowSamples) {
        m->sampleInRow = 0;
        if (++m->row >= m->len) m->row = 0;
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
