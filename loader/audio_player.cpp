#include <vitasdk.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sndfile.h>
#include "soloud.h"
#include "soloud_wavstream.h"
#include "soloud_wav.h"
#include "soloud_bus.h"

const char *mus_names[] = {
	"1-boss23",
	"1-dung73",
	"1-hero2",
	"1-plaintest2",
	"2cave2",
	"gameover",
	"heartbeat",
	"momentofwonder",
	"squire_new2"
};

SoLoud::Soloud soloud;
SoLoud::WavStream music[9];
SoLoud::WavStream *cur_music = nullptr;
int mus_handle = -1;

extern "C" {

void init_soloud() {
	soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF);
}

void resolve_music_name(char *name, char *out) {
	char *f = strdup(name);
	char *s = f;
	while (*f) {
		if (*f == '-')
			*f = '_';
		f++;
	}
	sprintf(out, "ux0:data/swordigo/raw/music_%s.mp3", s);
	free(s);
}

void load_music(const char *fname) {
	printf("loading %s\n", fname);
	if (cur_music) {
		cur_music->stop();
	} else { // Preloading all music files at first load to avoid stutters
		char full_fname[256];
		for (int i = 0; i < sizeof(mus_names) / sizeof(*mus_names); i++) {
			resolve_music_name(mus_names[i], full_fname);
			music[i].load(full_fname);
		}
	}

	for (int i = 0; i < sizeof(mus_names) / sizeof(*mus_names); i++) {
		if (!strcmp(mus_names[i], fname)) {
			cur_music = &music[i];
			break;
		}
	}
}

void play_music() {
	mus_handle = soloud.playBackground(*cur_music);
}

void set_music_loop(int looping) {
	//printf("looping %x\n", looping);
	if (mus_handle != -1)
		soloud.setLooping(mus_handle, looping);
	cur_music->setLooping(looping);
}

void set_music_volume(float vol) {
	//printf("set volume to %f\n", vol);
	soloud.setGlobalVolume(vol);
}

void stop_music() {
	cur_music->stop();
}

void pause_music() {
	soloud.setPause(mus_handle, !soloud.getPause(mus_handle));
}

}