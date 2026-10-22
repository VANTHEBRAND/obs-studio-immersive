/*
 * transform-vectors: dumps scene-item transform test vectors from libobs.
 *
 * Creates scene items with known inputs, lets libobs compute the box and
 * draw transforms, and writes everything as JSON. The output is consumed by
 * clean-room re-implementations of the transform math (see
 * docs/immersive/scene-editing-behavior-spec.md, section 9).
 *
 * Usage:
 *   transform-vectors [--out DIR] [--data-path DIR] [--mode absolute|relative|both]
 *                     [--canvas WxH] [--seed N] [--random N] [--grouped N]
 */

#include <obs.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <util/platform.h>

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* A private source type with a fixed, settable size and no rendering.      */

struct vector_source {
	uint32_t width;
	uint32_t height;
};

static const char *vector_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "Immersive vector source";
}

static void vector_source_update(void *data, obs_data_t *settings)
{
	struct vector_source *s = data;
	s->width = (uint32_t)obs_data_get_int(settings, "width");
	s->height = (uint32_t)obs_data_get_int(settings, "height");
}

static void *vector_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct vector_source *s = bzalloc(sizeof(struct vector_source));
	UNUSED_PARAMETER(source);
	vector_source_update(s, settings);
	return s;
}

static void vector_source_destroy(void *data)
{
	bfree(data);
}

static uint32_t vector_source_get_width(void *data)
{
	return ((struct vector_source *)data)->width;
}

static uint32_t vector_source_get_height(void *data)
{
	return ((struct vector_source *)data)->height;
}

static void vector_source_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(effect);
}

static struct obs_source_info vector_source_info = {
	.id = "immersive_vector_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = vector_source_get_name,
	.create = vector_source_create,
	.destroy = vector_source_destroy,
	.update = vector_source_update,
	.get_width = vector_source_get_width,
	.get_height = vector_source_get_height,
	.video_render = vector_source_render,
};

/* ------------------------------------------------------------------------ */
/* Inputs and JSON helpers.                                                 */

struct xf_input {
	uint32_t source_width;
	uint32_t source_height;
	struct vec2 pos;
	float rot;
	struct vec2 scale;
	uint32_t align;
	enum obs_bounds_type bounds_type;
	uint32_t bounds_align;
	struct vec2 bounds;
	struct obs_sceneitem_crop crop;
	bool crop_to_bounds;
};

static const char *bounds_type_names[] = {
	"OBS_BOUNDS_NONE",           "OBS_BOUNDS_STRETCH",        "OBS_BOUNDS_SCALE_INNER", "OBS_BOUNDS_SCALE_OUTER",
	"OBS_BOUNDS_SCALE_TO_WIDTH", "OBS_BOUNDS_SCALE_TO_HEIGHT", "OBS_BOUNDS_MAX_ONLY",
};

static const uint32_t alignments[9] = {
	OBS_ALIGN_CENTER,
	OBS_ALIGN_LEFT,
	OBS_ALIGN_RIGHT,
	OBS_ALIGN_TOP,
	OBS_ALIGN_BOTTOM,
	OBS_ALIGN_TOP | OBS_ALIGN_LEFT,
	OBS_ALIGN_TOP | OBS_ALIGN_RIGHT,
	OBS_ALIGN_BOTTOM | OBS_ALIGN_LEFT,
	OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT,
};

static void jf(FILE *f, float v)
{
	if (isnan(v) || isinf(v))
		fputs("null", f);
	else
		fprintf(f, "%.9g", (double)v);
}

static void jvec2(FILE *f, const struct vec2 *v)
{
	fputc('[', f);
	jf(f, v->x);
	fputc(',', f);
	jf(f, v->y);
	fputc(']', f);
}

static void jmatrix(FILE *f, const struct matrix4 *m)
{
	const struct vec4 *rows[4] = {&m->x, &m->y, &m->z, &m->t};
	fputc('[', f);
	for (int r = 0; r < 4; r++) {
		const float comps[4] = {rows[r]->x, rows[r]->y, rows[r]->z, rows[r]->w};
		for (int c = 0; c < 4; c++) {
			if (r || c)
				fputc(',', f);
			jf(f, comps[c]);
		}
	}
	fputc(']', f);
}

static void jcorners(FILE *f, const struct vec3 c[4])
{
	fputc('[', f);
	for (int i = 0; i < 4; i++) {
		if (i)
			fputc(',', f);
		fputc('[', f);
		jf(f, c[i].x);
		fputc(',', f);
		jf(f, c[i].y);
		fputc(']', f);
	}
	fputc(']', f);
}

static void jcrop(FILE *f, const struct obs_sceneitem_crop *crop)
{
	fprintf(f, "{\"left\":%d,\"top\":%d,\"right\":%d,\"bottom\":%d}", crop->left, crop->top, crop->right,
		crop->bottom);
}

static void jinput_body(FILE *f, const struct xf_input *in)
{
	fprintf(f, "\"source_width\":%" PRIu32 ",\"source_height\":%" PRIu32 ",\"position\":", in->source_width,
		in->source_height);
	jvec2(f, &in->pos);
	fputs(",\"rotation\":", f);
	jf(f, in->rot);
	fputs(",\"scale\":", f);
	jvec2(f, &in->scale);
	fprintf(f, ",\"alignment\":%" PRIu32 ",\"bounds_type\":\"%s\",\"bounds_alignment\":%" PRIu32 ",\"bounds\":",
		in->align, bounds_type_names[in->bounds_type], in->bounds_align);
	jvec2(f, &in->bounds);
	fputs(",\"crop\":", f);
	jcrop(f, &in->crop);
	fprintf(f, ",\"crop_to_bounds\":%s", in->crop_to_bounds ? "true" : "false");
}

static void jinput(FILE *f, const struct xf_input *in)
{
	fputc('{', f);
	jinput_body(f, in);
	fputc('}', f);
}

/* What libobs reports back for an item after the inputs were applied. */
static void jreported(FILE *f, obs_sceneitem_t *item)
{
	struct obs_transform_info info;
	struct obs_sceneitem_crop crop;
	obs_source_t *source = obs_sceneitem_get_source(item);

	obs_sceneitem_get_info2(item, &info);
	obs_sceneitem_get_crop(item, &crop);

	fputs("{\"position\":", f);
	jvec2(f, &info.pos);
	fputs(",\"rotation\":", f);
	jf(f, info.rot);
	fputs(",\"scale\":", f);
	jvec2(f, &info.scale);
	fprintf(f, ",\"alignment\":%" PRIu32 ",\"bounds_type\":\"%s\",\"bounds_alignment\":%" PRIu32 ",\"bounds\":",
		info.alignment, bounds_type_names[info.bounds_type], info.bounds_alignment);
	jvec2(f, &info.bounds);
	fputs(",\"crop\":", f);
	jcrop(f, &crop);
	fprintf(f, ",\"crop_to_bounds\":%s,\"source_width\":%" PRIu32 ",\"source_height\":%" PRIu32 "}",
		info.crop_to_bounds ? "true" : "false", obs_source_get_width(source), obs_source_get_height(source));
}

static void box_corners(const struct matrix4 *box, struct vec3 out[4])
{
	static const float unit[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
	for (int i = 0; i < 4; i++) {
		vec3_set(&out[i], unit[i][0], unit[i][1], 0.0f);
		vec3_transform(&out[i], &out[i], box);
	}
}

/* ------------------------------------------------------------------------ */
/* Applying inputs.                                                         */

static void apply_input(obs_sceneitem_t *item, const struct xf_input *in)
{
	struct obs_transform_info info = {0};
	info.pos = in->pos;
	info.rot = in->rot;
	info.scale = in->scale;
	info.alignment = in->align;
	info.bounds_type = in->bounds_type;
	info.bounds_alignment = in->bounds_align;
	info.bounds = in->bounds;
	info.crop_to_bounds = in->crop_to_bounds;

	obs_sceneitem_set_info2(item, &info);
	obs_sceneitem_set_crop(item, &in->crop);
	obs_sceneitem_force_update_transform(item);
}

static obs_source_t *create_vector_source(const char *name, uint32_t width, uint32_t height)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "width", width);
	obs_data_set_int(settings, "height", height);
	obs_source_t *source = obs_source_create_private(vector_source_info.id, name, settings);
	obs_data_release(settings);
	return source;
}

/* ------------------------------------------------------------------------ */
/* Deterministic pseudo-random generator (64-bit LCG), so a seed pins the   */
/* exact case list regardless of platform libc.                             */

static uint64_t rng_state;

static uint32_t rng_next(void)
{
	rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return (uint32_t)(rng_state >> 33);
}

static float rng_float(float lo, float hi)
{
	return lo + (hi - lo) * ((float)rng_next() / 2147483648.0f);
}

static int rng_int(int lo, int hi)
{
	return lo + (int)(rng_next() % (uint32_t)(hi - lo + 1));
}

static float rng_quarter(float lo, float hi)
{
	return floorf(rng_float(lo, hi) * 4.0f) / 4.0f;
}

static void random_input(struct xf_input *in)
{
	static const uint32_t sizes[] = {1920, 1280, 1080, 720, 640, 360, 333, 100, 17, 1};
	const size_t nsizes = sizeof(sizes) / sizeof(sizes[0]);

	memset(in, 0, sizeof(*in));
	in->source_width = sizes[rng_next() % nsizes];
	in->source_height = sizes[rng_next() % nsizes];
	in->pos.x = rng_quarter(-500.0f, 2500.0f);
	in->pos.y = rng_quarter(-500.0f, 1500.0f);
	in->rot = (rng_next() % 4 == 0) ? (float)rng_int(-8, 8) * 45.0f : rng_quarter(-360.0f, 360.0f);
	in->scale.x = rng_float(0.05f, 4.0f) * ((rng_next() % 5 == 0) ? -1.0f : 1.0f);
	in->scale.y = (rng_next() % 3 == 0) ? in->scale.x : rng_float(0.05f, 4.0f) * ((rng_next() % 5 == 0) ? -1.0f : 1.0f);
	in->align = alignments[rng_next() % 9];
	in->bounds_type = (enum obs_bounds_type)(rng_next() % 7);
	in->bounds_align = alignments[rng_next() % 9];
	in->bounds.x = rng_quarter(1.0f, 2000.0f);
	in->bounds.y = rng_quarter(1.0f, 2000.0f);
	if (rng_next() % 2) {
		in->crop.left = rng_int(0, 300);
		in->crop.top = rng_int(0, 300);
		in->crop.right = rng_int(0, 300);
		in->crop.bottom = rng_int(0, 300);
	}
	if (rng_next() % 50 == 0) {
		/* Over-crop: crop sum exceeds the source. */
		in->crop.left = (int)in->source_width;
		in->crop.right = (int)in->source_width;
	}
	in->crop_to_bounds = (rng_next() % 2) != 0;
}

/* ------------------------------------------------------------------------ */
/* Case emitters.                                                           */

struct emitter {
	FILE *f;
	int id;
	obs_scene_t *scene;
};

static void emit_case_begin(struct emitter *e, const char *kind, const char *name)
{
	if (e->id)
		fputs(",\n", e->f);
	fprintf(e->f, "{\"id\":%d,\"name\":\"%s\",\"kind\":\"%s\",", e->id, name, kind);
	e->id++;
}

static void emit_item_case(struct emitter *e, obs_sceneitem_t *item, const struct xf_input *in, const char *name)
{
	struct matrix4 box, draw;
	struct vec2 box_scale;
	struct vec3 corners[4];

	apply_input(item, in);

	obs_sceneitem_get_box_transform(item, &box);
	obs_sceneitem_get_draw_transform(item, &draw);
	obs_sceneitem_get_box_scale(item, &box_scale);
	box_corners(&box, corners);

	emit_case_begin(e, "item", name);
	fputs("\"input\":", e->f);
	jinput(e->f, in);
	fputs(",\"reported\":", e->f);
	jreported(e->f, item);
	fputs(",\"output\":{\"box_scale\":", e->f);
	jvec2(e->f, &box_scale);
	fputs(",\"box_matrix\":", e->f);
	jmatrix(e->f, &box);
	fputs(",\"draw_matrix\":", e->f);
	jmatrix(e->f, &draw);
	fputs(",\"corners\":", e->f);
	jcorners(e->f, corners);
	fputs(",\"canvas_corners\":", e->f);
	jcorners(e->f, corners);
	fputs("}}", e->f);
}

/* Creates a scene with a group and one child, applies the child's transform
 * in canvas space, then groups it, and records both sides. */
static void emit_grouped_case(struct emitter *e, const struct xf_input *child_in, const struct xf_input *group_in,
			      const char *name)
{
	obs_scene_t *scene = obs_scene_create_private("vector-group-scene");
	obs_sceneitem_t *group = obs_scene_add_group(scene, "vector-group");
	obs_source_t *source = create_vector_source("vector-child", child_in->source_width, child_in->source_height);
	obs_sceneitem_t *child = obs_scene_add(scene, source);

	struct matrix4 pre_box, child_box, child_draw, group_box, group_draw;
	struct vec2 child_box_scale;
	struct vec3 pre_corners[4], corners[4], canvas_corners[4];

	apply_input(group, group_in);
	apply_input(child, child_in);

	obs_sceneitem_get_box_transform(child, &pre_box);
	box_corners(&pre_box, pre_corners);

	obs_sceneitem_group_add_item(group, child);
	obs_sceneitem_force_update_transform(child);
	obs_sceneitem_force_update_transform(group);

	obs_sceneitem_get_box_transform(child, &child_box);
	obs_sceneitem_get_draw_transform(child, &child_draw);
	obs_sceneitem_get_box_scale(child, &child_box_scale);
	obs_sceneitem_get_box_transform(group, &group_box);
	obs_sceneitem_get_draw_transform(group, &group_draw);

	box_corners(&child_box, corners);
	for (int i = 0; i < 4; i++)
		vec3_transform(&canvas_corners[i], &corners[i], &group_draw);

	emit_case_begin(e, "grouped_item", name);
	fputs("\"input\":{", e->f);
	jinput_body(e->f, child_in);
	fputs(",\"group\":", e->f);
	jinput(e->f, group_in);
	fputc('}', e->f);
	fputs(",\"reported\":", e->f);
	jreported(e->f, child);
	fputs(",\"output\":{\"box_scale\":", e->f);
	jvec2(e->f, &child_box_scale);
	fputs(",\"box_matrix\":", e->f);
	jmatrix(e->f, &child_box);
	fputs(",\"draw_matrix\":", e->f);
	jmatrix(e->f, &child_draw);
	fputs(",\"corners\":", e->f);
	jcorners(e->f, corners);
	fputs(",\"canvas_corners\":", e->f);
	jcorners(e->f, canvas_corners);
	fputs(",\"pre_group_corners\":", e->f);
	jcorners(e->f, pre_corners);
	fputs(",\"group\":{\"reported\":", e->f);
	jreported(e->f, group);
	fprintf(e->f, ",\"source_width\":%" PRIu32 ",\"source_height\":%" PRIu32 ",\"box_matrix\":",
		obs_source_get_width(obs_sceneitem_get_source(group)),
		obs_source_get_height(obs_sceneitem_get_source(group)));
	jmatrix(e->f, &group_box);
	fputs(",\"draw_matrix\":", e->f);
	jmatrix(e->f, &group_draw);
	fputs("}}}", e->f);

	obs_source_release(source);
	obs_scene_release(scene);
}

/* ------------------------------------------------------------------------ */

struct options {
	const char *out_dir;
	const char *data_path;
	const char *mode;
	uint32_t canvas_width;
	uint32_t canvas_height;
	uint64_t seed;
	int random_count;
	int grouped_count;
};

static const char *platform_name(void)
{
#if defined(_WIN32)
	return "windows";
#elif defined(__APPLE__) && defined(__aarch64__)
	return "macos-arm64";
#elif defined(__APPLE__)
	return "macos-x86_64";
#elif defined(__aarch64__)
	return "linux-arm64";
#else
	return "linux-x86_64";
#endif
}

static void run_mode(const struct options *opt, bool absolute, bool have_video)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/transform-vectors-%s.json", opt->out_dir, absolute ? "absolute" : "relative");

	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "cannot open %s for writing\n", path);
		return;
	}

	obs_data_t *priv = obs_data_create();
	obs_data_set_bool(priv, "AbsoluteCoordinates", absolute);
	obs_apply_private_data(priv);
	obs_data_release(priv);

	fprintf(f,
		"{\"generator\":{\"libobs_version\":\"%s\",\"platform\":\"%s\",\"canvas\":{\"width\":%" PRIu32
		",\"height\":%" PRIu32 "},\"coordinate_mode\":\"%s\",\"seed\":%" PRIu64 "},\n\"cases\":[\n",
		obs_get_version_string(), platform_name(), opt->canvas_width, opt->canvas_height,
		absolute ? "absolute" : "relative", opt->seed);

	struct emitter e = {.f = f, .id = 0};
	e.scene = obs_scene_create_private("vector-scene");

	/* Exhaustive core: every alignment x bounds type x bounds alignment,
	 * with a few scales, one crop and both crop-to-bounds values. */
	{
		struct vec2 scales[3];
		vec2_set(&scales[0], 1.0f, 1.0f);
		vec2_set(&scales[1], -1.0f, 1.0f);
		vec2_set(&scales[2], 0.5f, 0.75f);
		obs_source_t *source = create_vector_source("vector-core", 640, 360);
		obs_sceneitem_t *item = obs_scene_add(e.scene, source);
		char name[128];

		for (int a = 0; a < 9; a++) {
			for (int bt = 0; bt < 7; bt++) {
				for (int ba = 0; ba < 9; ba++) {
					for (size_t s = 0; s < 3; s++) {
						for (int ctb = 0; ctb < 2; ctb++) {
							struct xf_input in = {0};
							in.source_width = 640;
							in.source_height = 360;
							vec2_set(&in.pos, 300.25f, 200.5f);
							in.rot = 0.0f;
							in.scale = scales[s];
							in.align = alignments[a];
							in.bounds_type = (enum obs_bounds_type)bt;
							in.bounds_align = alignments[ba];
							vec2_set(&in.bounds, 500.0f, 400.0f);
							in.crop.left = 10;
							in.crop.top = 20;
							in.crop.right = 30;
							in.crop.bottom = 40;
							in.crop_to_bounds = ctb != 0;
							snprintf(name, sizeof(name), "core a%d bt%d ba%d s%zu ctb%d", a, bt, ba, s,
								 ctb);
							emit_item_case(&e, item, &in, name);
						}
					}
				}
			}
		}

		obs_sceneitem_remove(item);
		obs_source_release(source);
	}

	/* Seeded random sweep. One source and item per distinct size pair. */
	{
		rng_state = opt->seed;
		for (int i = 0; i < opt->random_count; i++) {
			struct xf_input in;
			char name[64];
			random_input(&in);
			obs_source_t *source = create_vector_source("vector-random", in.source_width, in.source_height);
			obs_sceneitem_t *item = obs_scene_add(e.scene, source);
			snprintf(name, sizeof(name), "random %d", i);
			emit_item_case(&e, item, &in, name);
			obs_sceneitem_remove(item);
			obs_source_release(source);
		}
	}

	/* Grouped cases need the main canvas, which needs video. */
	if (have_video) {
		rng_state = opt->seed ^ 0x9e3779b97f4a7c15ULL;
		for (int i = 0; i < opt->grouped_count; i++) {
			struct xf_input child, group;
			char name[64];
			random_input(&child);
			random_input(&group);
			/* Keep groups to the configurations OBS actually produces. */
			group.source_width = 0;
			group.source_height = 0;
			memset(&group.crop, 0, sizeof(group.crop));
			group.crop_to_bounds = false;
			if (rng_next() % 4 != 0)
				group.bounds_type = OBS_BOUNDS_NONE;
			snprintf(name, sizeof(name), "grouped %d", i);
			emit_grouped_case(&e, &child, &group, name);
		}
	} else {
		fprintf(stderr, "video not initialized: grouped cases skipped for %s mode\n",
			absolute ? "absolute" : "relative");
	}

	obs_scene_release(e.scene);

	fputs("\n]}\n", f);
	fclose(f);
	fprintf(stderr, "wrote %d cases to %s\n", e.id, path);
}

static void usage(void)
{
	fputs("usage: transform-vectors [--out DIR] [--data-path DIR] [--mode absolute|relative|both]\n"
	      "                         [--canvas WxH] [--seed N] [--random N] [--grouped N]\n",
	      stderr);
}

int main(int argc, char **argv)
{
	struct options opt = {
		.out_dir = ".",
		.data_path = NULL,
		.mode = "both",
		.canvas_width = 1920,
		.canvas_height = 1080,
		.seed = 20261022ULL,
		.random_count = 3000,
		.grouped_count = 300,
	};

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
		if (strcmp(arg, "--out") == 0 && val) {
			opt.out_dir = val;
			i++;
		} else if (strcmp(arg, "--data-path") == 0 && val) {
			opt.data_path = val;
			i++;
		} else if (strcmp(arg, "--mode") == 0 && val) {
			opt.mode = val;
			i++;
		} else if (strcmp(arg, "--canvas") == 0 && val) {
			if (sscanf(val, "%" SCNu32 "x%" SCNu32, &opt.canvas_width, &opt.canvas_height) != 2) {
				usage();
				return 1;
			}
			i++;
		} else if (strcmp(arg, "--seed") == 0 && val) {
			opt.seed = strtoull(val, NULL, 10);
			i++;
		} else if (strcmp(arg, "--random") == 0 && val) {
			opt.random_count = atoi(val);
			i++;
		} else if (strcmp(arg, "--grouped") == 0 && val) {
			opt.grouped_count = atoi(val);
			i++;
		} else {
			usage();
			return 1;
		}
	}

	if (!obs_startup("en-US", NULL, NULL)) {
		fputs("obs_startup failed\n", stderr);
		return 1;
	}

	if (opt.data_path)
		obs_add_data_path(opt.data_path);

	obs_register_source(&vector_source_info);

	struct obs_video_info ovi = {0};
	ovi.adapter = 0;
	ovi.fps_num = 30;
	ovi.fps_den = 1;
#if defined(_WIN32)
	ovi.graphics_module = "libobs-d3d11";
#else
	ovi.graphics_module = "libobs-opengl";
#endif
	ovi.output_format = VIDEO_FORMAT_RGBA;
	ovi.base_width = opt.canvas_width;
	ovi.base_height = opt.canvas_height;
	ovi.output_width = opt.canvas_width;
	ovi.output_height = opt.canvas_height;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_FULL;
	ovi.scale_type = OBS_SCALE_BILINEAR;

	int video_result = obs_reset_video(&ovi);
	bool have_video = video_result == OBS_VIDEO_SUCCESS;
	if (!have_video) {
		fprintf(stderr,
			"obs_reset_video failed (%d). Relative coordinate mode and grouped cases need the main "
			"canvas and are skipped; pass --data-path <repo>/libobs/data if the effects were not found.\n",
			video_result);
	}

	bool do_absolute = strcmp(opt.mode, "absolute") == 0 || strcmp(opt.mode, "both") == 0;
	bool do_relative = strcmp(opt.mode, "relative") == 0 || strcmp(opt.mode, "both") == 0;

	if (do_absolute)
		run_mode(&opt, true, have_video);
	if (do_relative) {
		if (have_video)
			run_mode(&opt, false, have_video);
		else
			fputs("relative coordinate mode skipped: no video\n", stderr);
	}

	obs_shutdown();
	return 0;
}
