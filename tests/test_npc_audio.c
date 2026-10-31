/* Exercise the actual mixer channel calculation, including native volume fades. */
#include <assert.h>
#include <math.h>
#include "../src/audio/effects.c"

struct SequencePlayer gSequencePlayers[SEQUENCE_PLAYERS];
static f32 speechScale = 1.0f;
f32 ai_npc_music_volume(void) { return speechScale; }

int main(void) {
    struct SequenceChannel channel = {0};
    struct SequenceChannelLayer layer = {0};
    struct Note note = {0};
    s32 player;
    channel.volume = 0.8f; channel.volumeScale = 0.75f;
    channel.freqScale = layer.freqScale = layer.velocitySquare = 1.0f;
    channel.layers[0] = &layer; layer.enabled = TRUE; layer.note = &note;
    for (player = 0; player < SEQUENCE_PLAYERS; player++) {
        channel.seqPlayer = &gSequencePlayers[player];
        channel.seqPlayer->fadeVolume = 0.4f; /* Keep Bowser/cutscene lowering intact. */
        speechScale = 0.5f;
        sequence_channel_process_sound(&channel);
        assert(fabsf(layer.noteVelocity - (player == 0 ? 0.12f : 0.24f)) < 0.0001f);
        assert(channel.seqPlayer->fadeVolume == 0.4f);
        speechScale = 1.0f;
        sequence_channel_process_sound(&channel);
        assert(fabsf(layer.noteVelocity - 0.24f) < 0.0001f);
        assert(channel.seqPlayer->fadeVolume == 0.4f);
    }
    return 0;
}
