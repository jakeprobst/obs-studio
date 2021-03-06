/*
Copyright (C) 2014 by Leonhard Oelke <leonhard@in-verted.de>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <math.h>

#include "util/threading.h"
#include "util/bmem.h"
#include "obs.h"
#include "obs-internal.h"

#include "obs-audio-controls.h"

/* These are pointless warnings generated not by our code, but by a standard
 * library macro, INFINITY */
#ifdef _MSC_VER
#pragma warning(disable : 4056)
#pragma warning(disable : 4756)
#endif

typedef float (*obs_fader_conversion_t)(const float val);

struct obs_fader {
	pthread_mutex_t        mutex;
	signal_handler_t       *signals;
	obs_fader_conversion_t def_to_db;
	obs_fader_conversion_t db_to_def;
	obs_source_t           *source;
	enum obs_fader_type    type;
	float                  max_db;
	float                  min_db;
	float                  cur_db;
	bool                   ignore_next_signal;
};

struct obs_volmeter {
	pthread_mutex_t        mutex;
	signal_handler_t       *signals;
	obs_fader_conversion_t pos_to_db;
	obs_fader_conversion_t db_to_pos;
	obs_source_t           *source;
	enum obs_fader_type    type;
	float                  cur_db;
};

static const char *fader_signals[] = {
	"void volume_changed(ptr fader, float db)",
	NULL
};

static const char *volmeter_signals[] = {
	"void levels_updated(ptr volmeter, float level, "
			"float magnitude, float peak)",
	NULL
};

static inline float mul_to_db(const float mul)
{
	return (mul == 0.0f) ? -INFINITY : 20.0f * log10f(mul);
}

static inline float db_to_mul(const float db)
{
	return (db == -INFINITY) ? 0.0f : powf(10.0f, db / 20.0f);
}

static float cubic_def_to_db(const float def)
{
	if (def == 1.0f)
		return 0.0f;
	else if (def <= 0.0f)
		return -INFINITY;

	return mul_to_db(def * def * def);
}

static float cubic_db_to_def(const float db)
{
	if (db == 0.0f)
		return 1.0f;
	else if (db == -INFINITY)
		return 0.0f;

	return cbrtf(db_to_mul(db));
}

static float iec_def_to_db(const float def)
{
	if (def == 1.0f)
		return 0.0f;
	else if (def <= 0.0f)
		return -INFINITY;

	float db;

	if (def >= 0.75f)
		db = (def - 1.0f) / 0.25f * 9.0f;
	else if (def >= 0.5f)
		db = (def - 0.75f) / 0.25f * 11.0f - 9.0f;
	else if (def >= 0.3f)
		db = (def - 0.5f) / 0.2f * 10.0f - 20.0f;
	else if (def >= 0.15f)
		db = (def - 0.3f) / 0.15f * 10.0f - 30.0f;
	else if (def >= 0.075f)
		db = (def - 0.15f) / 0.075f * 10.0f - 40.0f;
	else if (def >= 0.025f)
		db = (def - 0.075f) / 0.05f * 10.0f - 50.0f;
	else if (def >= 0.001f)
		db = (def - 0.025f) / 0.025f * 90.0f - 60.0f;
	else
		db = -INFINITY;

	return db;
}

static float iec_db_to_def(const float db)
{
	if (db == 0.0f)
		return 1.0f;
	else if (db == -INFINITY)
		return 0.0f;

	float def;

	if (db >= -9.0f)
		def = (db + 9.0f) / 9.0f * 0.25f + 0.75f;
	else if (db >= -20.0f)
		def = (db + 20.0f) / 11.0f * 0.25f + 0.5f;
	else if (db >= -30.0f)
		def = (db + 30.0f) / 10.0f * 0.2f + 0.3f;
	else if (db >= -40.0f)
		def = (db + 40.0f) / 10.0f * 0.15f + 0.15f;
	else if (db >= -50.0f)
		def = (db + 50.0f) / 10.0f * 0.075f + 0.075f;
	else if (db >= -60.0f)
		def = (db + 60.0f) / 10.0f * 0.05f + 0.025f;
	else if (db >= -114.0f)
		def = (db + 150.0f) / 90.0f * 0.025f;
	else
		def = 0.0f;

	return def;
}

#define LOG_OFFSET_DB  6.0f
#define LOG_RANGE_DB   96.0f
/* equals -log10f(LOG_OFFSET_DB) */
#define LOG_OFFSET_VAL -0.77815125038364363f
/* equals -log10f(-LOG_RANGE_DB + LOG_OFFSET_DB) */
#define LOG_RANGE_VAL  -2.00860017176191756f

static float log_def_to_db(const float def)
{
	if (def >= 1.0f)
		return 0.0f;
	else if (def <= 0.0f)
		return -INFINITY;

	return -(LOG_RANGE_DB + LOG_OFFSET_DB) * powf(
			(LOG_RANGE_DB + LOG_OFFSET_DB) / LOG_OFFSET_DB, -def)
			+ LOG_OFFSET_DB;
}

static float log_db_to_def(const float db)
{
	if (db >= 0.0f)
		return 1.0f;
	else if (db <= -96.0f)
		return 0.0f;

	return (-log10f(-db + LOG_OFFSET_DB) - LOG_RANGE_VAL)
			/ (LOG_OFFSET_VAL - LOG_RANGE_VAL);
}

static void signal_volume_changed(signal_handler_t *sh,
		struct obs_fader *fader, const float db)
{
	struct calldata data;

	calldata_init(&data);

	calldata_set_ptr  (&data, "fader", fader);
	calldata_set_float(&data, "db",    db);

	signal_handler_signal(sh, "volume_changed", &data);

	calldata_free(&data);
}

static void signal_levels_updated(signal_handler_t *sh,
		struct obs_volmeter *volmeter,
		const float level, const float magnitude, const float peak)
{
	struct calldata data;

	calldata_init(&data);

	calldata_set_ptr  (&data, "volmeter",  volmeter);
	calldata_set_float(&data, "level",     level);
	calldata_set_float(&data, "magnitude", magnitude);
	calldata_set_float(&data, "peak",      peak);

	signal_handler_signal(sh, "levels_updated", &data);

	calldata_free(&data);
}

static void fader_source_volume_changed(void *vptr, calldata_t *calldata)
{
	struct obs_fader *fader = (struct obs_fader *) vptr;

	pthread_mutex_lock(&fader->mutex);

	if (fader->ignore_next_signal) {
		fader->ignore_next_signal = false;
		pthread_mutex_unlock(&fader->mutex);
		return;
	}

	signal_handler_t *sh = fader->signals;
	const float mul      = (float)calldata_float(calldata, "volume");
	const float db       = mul_to_db(mul);
	fader->cur_db        = db;

	pthread_mutex_unlock(&fader->mutex);

	signal_volume_changed(sh, fader, db);
}

static void volmeter_source_volume_changed(void *vptr, calldata_t *calldata)
{
	struct obs_volmeter *volmeter = (struct obs_volmeter *) vptr;

	pthread_mutex_lock(&volmeter->mutex);

	float mul = (float) calldata_float(calldata, "volume");
	volmeter->cur_db = mul_to_db(mul);

	pthread_mutex_unlock(&volmeter->mutex);
}

static void fader_source_destroyed(void *vptr, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	struct obs_fader *fader = (struct obs_fader *) vptr;

	obs_fader_detach_source(fader);
}

static void volmeter_source_volume_levels(void *vptr, calldata_t *calldata)
{
	struct obs_volmeter *volmeter = (struct obs_volmeter *) vptr;

	pthread_mutex_lock(&volmeter->mutex);

	float mul = db_to_mul(volmeter->cur_db);

	float level     = (float) calldata_float(calldata, "level");
	float magnitude = (float) calldata_float(calldata, "magnitude");
	float peak      = (float) calldata_float(calldata, "peak");

	level     = volmeter->db_to_pos(mul_to_db(level     * mul));
	magnitude = volmeter->db_to_pos(mul_to_db(magnitude * mul));
	peak      = volmeter->db_to_pos(mul_to_db(peak      * mul));

	signal_handler_t *sh = volmeter->signals;

	pthread_mutex_unlock(&volmeter->mutex);

	signal_levels_updated(sh, volmeter, level, magnitude, peak);
}

static void volmeter_source_destroyed(void *vptr, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	struct obs_volmeter *volmeter = (struct obs_volmeter *) vptr;

	obs_volmeter_detach_source(volmeter);
}

obs_fader_t *obs_fader_create(enum obs_fader_type type)
{
	struct obs_fader *fader = bzalloc(sizeof(struct obs_fader));
	if (!fader)
		return NULL;

	pthread_mutex_init_value(&fader->mutex);
	if (pthread_mutex_init(&fader->mutex, NULL) != 0)
		goto fail;
	fader->signals = signal_handler_create();
	if (!fader->signals)
		goto fail;
	if (!signal_handler_add_array(fader->signals, fader_signals))
		goto fail;

	switch(type) {
	case OBS_FADER_CUBIC:
		fader->def_to_db = cubic_def_to_db;
		fader->db_to_def = cubic_db_to_def;
		fader->max_db    = 0.0f;
		fader->min_db    = -INFINITY;
		break;
	case OBS_FADER_IEC:
		fader->def_to_db = iec_def_to_db;
		fader->db_to_def = iec_db_to_def;
		fader->max_db    = 0.0f;
		fader->min_db    = -INFINITY;
		break;
	case OBS_FADER_LOG:
		fader->def_to_db = log_def_to_db;
		fader->db_to_def = log_db_to_def;
		fader->max_db    = 0.0f;
		fader->min_db    = -96.0f;
		break;
	default:
		goto fail;
		break;
	}
	fader->type = type;

	return fader;
fail:
	obs_fader_destroy(fader);
	return NULL;
}

void obs_fader_destroy(obs_fader_t *fader)
{
	if (!fader)
		return;

	obs_fader_detach_source(fader);
	signal_handler_destroy(fader->signals);
	pthread_mutex_destroy(&fader->mutex);

	bfree(fader);
}

bool obs_fader_set_db(obs_fader_t *fader, const float db)
{
	if (!fader)
		return false;

	pthread_mutex_lock(&fader->mutex);

	bool clamped  = false;
	fader->cur_db = db;

	if (fader->cur_db > fader->max_db) {
		fader->cur_db = fader->max_db;
		clamped       = true;
	}
	if (fader->cur_db < fader->min_db) {
		fader->cur_db = -INFINITY;
		clamped       = true;
	}

	fader->ignore_next_signal = true;
	obs_source_t *src         = fader->source;
	const float mul           = db_to_mul(fader->cur_db);

	pthread_mutex_unlock(&fader->mutex);

	if (src)
		obs_source_set_volume(src, mul);

	return !clamped;
}

float obs_fader_get_db(obs_fader_t *fader)
{
	if (!fader)
		return 0.0f;

	pthread_mutex_lock(&fader->mutex);
	const float db = fader->cur_db;
	pthread_mutex_unlock(&fader->mutex);

	return db;
}

bool obs_fader_set_deflection(obs_fader_t *fader, const float def)
{
	if (!fader)
		return false;

	return obs_fader_set_db(fader, fader->def_to_db(def));
}

float obs_fader_get_deflection(obs_fader_t *fader)
{
	if (!fader)
		return 0.0f;

	pthread_mutex_lock(&fader->mutex);
	const float def = fader->db_to_def(fader->cur_db);
	pthread_mutex_unlock(&fader->mutex);

	return def;
}

bool obs_fader_set_mul(obs_fader_t *fader, const float mul)
{
	if (!fader)
		return false;

	return obs_fader_set_db(fader, mul_to_db(mul));
}

float obs_fader_get_mul(obs_fader_t *fader)
{
	if (!fader)
		return 0.0f;

	pthread_mutex_lock(&fader->mutex);
	const float mul = db_to_mul(fader->cur_db);
	pthread_mutex_unlock(&fader->mutex);

	return mul;
}

bool obs_fader_attach_source(obs_fader_t *fader, obs_source_t *source)
{
	signal_handler_t *sh;

	if (!fader || !source)
		return false;

	obs_fader_detach_source(fader);

	pthread_mutex_lock(&fader->mutex);

	sh = obs_source_get_signal_handler(source);
	signal_handler_connect(sh, "volume",
			fader_source_volume_changed, fader);
	signal_handler_connect(sh, "destroy",
			fader_source_destroyed, fader);

	fader->source = source;
	fader->cur_db = mul_to_db(obs_source_get_volume(source));

	pthread_mutex_unlock(&fader->mutex);

	return true;
}

void obs_fader_detach_source(obs_fader_t *fader)
{
	signal_handler_t *sh;

	if (!fader)
		return;

	pthread_mutex_lock(&fader->mutex);

	if (!fader->source)
		goto exit;

	sh = obs_source_get_signal_handler(fader->source);
	signal_handler_disconnect(sh, "volume",
			fader_source_volume_changed, fader);
	signal_handler_disconnect(sh, "destroy",
			fader_source_destroyed, fader);

	fader->source = NULL;

exit:
	pthread_mutex_unlock(&fader->mutex);
}

signal_handler_t *obs_fader_get_signal_handler(obs_fader_t *fader)
{
	return (fader) ? fader->signals : NULL;
}

obs_volmeter_t *obs_volmeter_create(enum obs_fader_type type)
{
	struct obs_volmeter *volmeter = bzalloc(sizeof(struct obs_volmeter));
	if (!volmeter)
		return NULL;

	pthread_mutex_init_value(&volmeter->mutex);
	if (pthread_mutex_init(&volmeter->mutex, NULL) != 0)
		goto fail;
	volmeter->signals = signal_handler_create();
	if (!volmeter->signals)
		goto fail;
	if (!signal_handler_add_array(volmeter->signals, volmeter_signals))
		goto fail;

	/* set conversion functions */
	switch(type) {
	case OBS_FADER_CUBIC:
		volmeter->pos_to_db = cubic_def_to_db;
		volmeter->db_to_pos = cubic_db_to_def;
		break;
	case OBS_FADER_IEC:
		volmeter->pos_to_db = iec_def_to_db;
		volmeter->db_to_pos = iec_db_to_def;
		break;
	case OBS_FADER_LOG:
		volmeter->pos_to_db = log_def_to_db;
		volmeter->db_to_pos = log_db_to_def;
		break;
	default:
		goto fail;
		break;
	}
	volmeter->type = type;

	return volmeter;
fail:
	obs_volmeter_destroy(volmeter);
	return NULL;
}

void obs_volmeter_destroy(obs_volmeter_t *volmeter)
{
	if (!volmeter)
		return;

	obs_volmeter_detach_source(volmeter);
	signal_handler_destroy(volmeter->signals);
	pthread_mutex_destroy(&volmeter->mutex);

	bfree(volmeter);
}

bool obs_volmeter_attach_source(obs_volmeter_t *volmeter, obs_source_t *source)
{
	signal_handler_t *sh;

	if (!volmeter || !source)
		return false;

	obs_volmeter_detach_source(volmeter);

	pthread_mutex_lock(&volmeter->mutex);

	sh = obs_source_get_signal_handler(source);
	signal_handler_connect(sh, "volume",
			volmeter_source_volume_changed, volmeter);
	signal_handler_connect(sh, "volume_level",
			volmeter_source_volume_levels, volmeter);
	signal_handler_connect(sh, "destroy",
			volmeter_source_destroyed, volmeter);

	volmeter->source = source;
	volmeter->cur_db = mul_to_db(obs_source_get_volume(source));

	pthread_mutex_unlock(&volmeter->mutex);

	return true;
}

void obs_volmeter_detach_source(obs_volmeter_t *volmeter)
{
	signal_handler_t *sh;

	if (!volmeter)
		return;

	pthread_mutex_lock(&volmeter->mutex);

	if (!volmeter->source)
		goto exit;

	sh = obs_source_get_signal_handler(volmeter->source);
	signal_handler_disconnect(sh, "volume",
			volmeter_source_volume_changed, volmeter);
	signal_handler_disconnect(sh, "volume_level",
			volmeter_source_volume_levels, volmeter);
	signal_handler_disconnect(sh, "destroy",
			volmeter_source_destroyed, volmeter);

	volmeter->source = NULL;

exit:
	pthread_mutex_unlock(&volmeter->mutex);
}

signal_handler_t *obs_volmeter_get_signal_handler(obs_volmeter_t *volmeter)
{
	return (volmeter) ? volmeter->signals : NULL;
}

