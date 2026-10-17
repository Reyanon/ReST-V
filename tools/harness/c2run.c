// System C / C2 PC harness (ADR-107): run frames, video/audio hashes, save screen (ppm) and sound (raw)
//   c2harness --roms DIR --game SET [--frames N] [--checkpoint N] [--shot F1,F2] [--out DIR] [--wav FILE]
//             [--coin F] [--start F] [--hold from,to,bits] [--savestate F:path] [--loadstate F:path]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "c2.h"

static unsigned long long fnv(unsigned long long h, const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 0x100000001b3ULL;
	return h;
}

static void save_ppm(const char *path, const uint32_t *fb, int w, int h, int pitch)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const uint32_t c = fb[y * pitch + x];
			const unsigned char px[3] = { (unsigned char)c, (unsigned char)(c >> 8), (unsigned char)(c >> 16) };
			fwrite(px, 1, 3, f);
		}
	fclose(f);
}

int main(int argc, char **argv)
{
	const char *roms = "roms", *game = NULL, *out = ".", *wav = NULL, *savest = NULL, *loadst = NULL;
	int frames = 600, checkpoint = 600, coin_at = -1, start_at = -1, hold_from = -1, hold_to = -1, hold_bits = 0;
	int save_at = -1, load_at = -1;
	char shots[512] = "";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--roms") && i + 1 < argc) roms = argv[++i];
		else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--checkpoint") && i + 1 < argc) checkpoint = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--shot") && i + 1 < argc) snprintf(shots, sizeof(shots), ",%s,", argv[++i]);
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
		else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
		else if (!strcmp(argv[i], "--coin") && i + 1 < argc) coin_at = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--start") && i + 1 < argc) start_at = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--hold") && i + 1 < argc) sscanf(argv[++i], "%d,%d,%i", &hold_from, &hold_to, &hold_bits);
		else if (!strcmp(argv[i], "--savestate") && i + 1 < argc) { char *a = argv[++i]; save_at = atoi(a); savest = strchr(a, ':') + 1; }
		else if (!strcmp(argv[i], "--loadstate") && i + 1 < argc) { char *a = argv[++i]; load_at = atoi(a); loadst = strchr(a, ':') + 1; }
	}
	if (!game) {
		fprintf(stderr, "usage: c2harness --roms DIR --game SET ...\n");
		return 2;
	}
	char err[256];
	if (c2_load(roms, game, err, sizeof(err)) != 0) {
		fprintf(stderr, "load failed: %s\n", err);
		return 1;
	}
	FILE *wf = wav ? fopen(wav, "wb") : NULL;
	unsigned long long vh = 0xcbf29ce484222325ULL, ah = 0xcbf29ce484222325ULL, samples = 0;
	clock_t t0 = clock(), tc = t0;
	static int16_t abuf[4096 * 2];
	for (int f = 1; f <= frames; f++) {
		if (load_at == f && c2_state_load(loadst) != 0)
			fprintf(stderr, "loadstate failed\n");
		uint8_t p1 = 0, sys = 0;
		if (coin_at >= 0 && f >= coin_at && f < coin_at + 6) sys |= 0x01;
		if (start_at >= 0 && f >= start_at && f < start_at + 6) sys |= 0x10;
		if (hold_from >= 0 && f >= hold_from && f < hold_to) p1 |= (uint8_t)hold_bits;
		c2_run_frame(p1, 0, sys);
		if (save_at == f && c2_state_save(savest) != 0)
			fprintf(stderr, "savestate failed\n");
		int w, h, pitch;
		const uint32_t *fb = c2_frame(&w, &h, &pitch);
		for (int y = 0; y < h; y++)
			vh = fnv(vh, fb + y * pitch, (size_t)w * 4);
		int n;
		while ((n = c2_audio(abuf, 4096)) > 0) {
			ah = fnv(ah, abuf, (size_t)n * 4);
			samples += (unsigned long long)n;
			if (wf)
				fwrite(abuf, 4, (size_t)n, wf);
		}
		char tag[16];
		snprintf(tag, sizeof(tag), ",%d,", f);
		if (shots[0] && strstr(shots, tag)) {
			char path[600];
			snprintf(path, sizeof(path), "%s/%s_%05d.ppm", out, game, f);
			save_ppm(path, fb, w, h, pitch);
		}
		if (checkpoint > 0 && f % checkpoint == 0) {
			const clock_t t = clock();
			printf("frame %5d %dx%d video %016llx audio %016llx samples %llu  %.1f fps\n", f, w, h, vh, ah, samples,
				checkpoint / ((double)(t - tc) / CLOCKS_PER_SEC + 1e-9));
			tc = t;
		}
	}
	if (wf)
		fclose(wf);
	printf("done %d frames in %.2fs\n", frames, (double)(clock() - t0) / CLOCKS_PER_SEC);
	c2_unload();
	return 0;
}
