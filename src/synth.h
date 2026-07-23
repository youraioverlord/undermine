/* UNDERMINE — SID-style software synth (DESIGN.md §4).
 * Lives in the output layer, NOT the sim: generating audio never touches
 * GameState, so determinism is preserved. Sample generation is pure and
 * device-free, so it can be exercised headlessly in --selftest. */
#ifndef SYNTH_H
#define SYNTH_H

#include <stdbool.h>

#define SYNTH_RATE   44100
#define SYNTH_VOICES 3         /* like the real SID: 3 voices */
#define SEG_MAX      8

typedef enum {
    SFX_JUMP = 0,
    SFX_LAND,
    SFX_COAL,
    SFX_QUOTA,
    SFX_DEATH,
    SFX_DESCEND,
    SFX_SLAM,
    SFX_COUNT
} SfxId;

typedef enum { SONG_TITLE = 0, SONG_INGAME, SONG_OVER, SONG_COUNT } SongId;

/* one note of an effect: waveform, frequency, length, level */
typedef struct { float freq; int wave; int ms; float vol; } SynthSeg;
typedef struct { SynthSeg seg[SEG_MAX]; int n; int priority; } SynthSfx;

typedef struct {
    bool  active;
    const SynthSfx *sfx;
    int   segIdx;
    int   sampleInSeg;
    float phase;
    unsigned int noise;   /* LFSR state */
    int   priority;
} SynthVoice;

/* Music: melody + bass, plus an optional third arpeggio voice (the classic
 * SID "chord on one voice" trick), rendered alongside the SFX voices. */
typedef struct {
    const int *mel, *bass;   /* per-row frequencies in Hz; 0 = rest */
    const int *arp;          /* per-row chord roots; negative = minor, 0 = rest */
    int   len;               /* rows in the loop */
    int   row, sampleInRow, rowSamples;
    int   melWave, bassWave;
    float melPhase, bassPhase, arpPhase;
    int   arpStep, arpStepSamp;   /* triad note cycled at ~50 Hz, SID-style */
    bool  loop;              /* false: a one-shot jingle (game over) ends itself */
    float pitch;             /* multiplier for the depth-based transpose */
    int   duckPos, duckTotal;     /* death jingle ducks the music, fading back */
    int   sweepPos, sweepTotal;   /* low-pass sweep on the 10-depth transpose */
    float lp;                     /* one-pole filter state for the sweep */
    bool  on;
} SynthMusic;

typedef struct {
    SynthVoice v[SYNTH_VOICES];
    SynthMusic music;
    int rate;
} Synth;

void synth_init(Synth *s);
void synth_trigger(Synth *s, SfxId id);
void synth_music_play(Synth *s, SongId id);
void synth_music_stop(Synth *s);
void synth_set_pitch(Synth *s, float mult);            /* 1.0 = normal pitch */
void synth_sweep(Synth *s);      /* 1s filter dip — the "descending dread" cue */
void synth_render(Synth *s, float *out, int frames);   /* mono float, [-1,1] */

#endif
