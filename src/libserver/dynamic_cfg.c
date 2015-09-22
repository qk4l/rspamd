/* Copyright (c) 2010-2012, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in the
 *         documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "rspamd.h"
#include "map.h"
#include "filter.h"
#include "dynamic_cfg.h"

struct config_json_buf {
	GString *buf;
	struct rspamd_config *cfg;
};

/**
 * Apply configuration to the specified configuration
 * @param conf_metrics
 * @param cfg
 */
static void
apply_dynamic_conf (const ucl_object_t *top, struct rspamd_config *cfg)
{
	gint test_act;
	const ucl_object_t *cur_elt, *cur_nm, *it_val;
	ucl_object_iter_t it = NULL;
	struct metric *real_metric;
	struct metric_action *cur_action;
	struct rspamd_symbol_def *s;

	while ((cur_elt = ucl_iterate_object (top, &it, true))) {
		if (ucl_object_type (cur_elt) != UCL_OBJECT) {
			msg_err ("loaded json array element is not an object");
			continue;
		}

		cur_nm = ucl_object_find_key (cur_elt, "metric");
		if (!cur_nm || ucl_object_type (cur_nm) != UCL_STRING) {
			msg_err (
					"loaded json metric object element has no 'metric' attribute");
			continue;
		}
		real_metric = g_hash_table_lookup (cfg->metrics,
							ucl_object_tostring (cur_nm));
		if (real_metric == NULL) {
			msg_warn ("cannot find metric %s", ucl_object_tostring (cur_nm));
			continue;
		}

		cur_nm = ucl_object_find_key (cur_elt, "symbols");
		/* Parse symbols */
		if (cur_nm && ucl_object_type (cur_nm) == UCL_ARRAY) {
			ucl_object_iter_t nit = NULL;

			while ((it_val = ucl_iterate_object (cur_nm, &nit, true))) {
				if (ucl_object_find_key (it_val, "name") &&
						ucl_object_find_key (it_val, "value")) {
					const ucl_object_t *n =
							ucl_object_find_key (it_val, "name");
					const ucl_object_t *v =
							ucl_object_find_key (it_val, "value");

					if((s = g_hash_table_lookup (real_metric->symbols,
							ucl_object_tostring (n))) != NULL) {
						*s->weight_ptr = ucl_object_todouble (v);
					}
				}
				else {
					msg_info (
							"json symbol object has no mandatory 'name' and 'value' attributes");
				}
			}
		}
		else {
			ucl_object_t *arr;

			arr = ucl_object_typed_new (UCL_ARRAY);
			ucl_object_insert_key ((ucl_object_t *)cur_elt, arr, "symbols",
					sizeof ("symbols") - 1, false);
		}
		cur_nm = ucl_object_find_key (cur_elt, "actions");
		/* Parse actions */
		if (cur_nm && ucl_object_type (cur_nm) == UCL_ARRAY) {
			ucl_object_iter_t nit = NULL;

			while ((it_val = ucl_iterate_object (cur_nm, &nit, true))) {
				if (ucl_object_find_key (it_val, "name") &&
						ucl_object_find_key (it_val, "value")) {
					if (!rspamd_action_from_str (ucl_object_tostring (
							ucl_object_find_key (it_val, "name")), &test_act)) {
						msg_err ("unknown action: %s",
								ucl_object_tostring (ucl_object_find_key (it_val,
										"name")));
						continue;
					}
					cur_action = &real_metric->actions[test_act];
					cur_action->action = test_act;
					cur_action->score =
							ucl_object_todouble (ucl_object_find_key (it_val,
									"value"));
				}
				else {
					msg_info (
							"json action object has no mandatory 'name' and 'value' attributes");
				}
			}
		}
		else {
			ucl_object_t *arr;

			arr = ucl_object_typed_new (UCL_ARRAY);
			ucl_object_insert_key ((ucl_object_t *)cur_elt, arr, "actions",
					sizeof ("actions") - 1, false);
		}
	}
}

/* Callbacks for reading json dynamic rules */
gchar *
json_config_read_cb (rspamd_mempool_t * pool,
	gchar * chunk,
	gint len,
	struct map_cb_data *data)
{
	struct config_json_buf *jb, *pd;

	pd = data->prev_data;

	g_assert (pd != NULL);

	if (data->cur_data == NULL) {
		jb = g_slice_alloc (sizeof (*jb));
		jb->cfg = pd->cfg;
		jb->buf = pd->buf;
		data->cur_data = jb;
	}
	else {
		jb = data->cur_data;
	}

	if (jb->buf == NULL) {
		/* Allocate memory for buffer */
		jb->buf = g_string_sized_new (BUFSIZ);
	}

	g_string_append_len (jb->buf, chunk, len);

	return NULL;
}

void
json_config_fin_cb (rspamd_mempool_t * pool, struct map_cb_data *data)
{
	struct config_json_buf *jb;
	ucl_object_t *top;
	struct ucl_parser *parser;

	if (data->prev_data) {
		jb = data->prev_data;
		/* Clean prev data */
		g_slice_free1 (sizeof (*jb), jb);
	}

	/* Now parse json */
	if (data->cur_data) {
		jb = data->cur_data;
	}
	else {
		msg_err ("no data read");
		return;
	}
	if (jb->buf == NULL) {
		msg_err ("no data read");
		return;
	}

	parser = ucl_parser_new (0);

	if (!ucl_parser_add_chunk (parser, jb->buf->str, jb->buf->len)) {
		msg_err ("cannot load json data: parse error %s",
				ucl_parser_get_error (parser));
		ucl_parser_free (parser);
		return;
	}

	top = ucl_parser_get_object (parser);
	ucl_parser_free (parser);

	if (ucl_object_type (top) != UCL_ARRAY) {
		ucl_object_unref (top);
		msg_err ("loaded json is not an array");
		return;
	}

	ucl_object_unref (jb->cfg->current_dynamic_conf);
	apply_dynamic_conf (top, jb->cfg);
	jb->cfg->current_dynamic_conf = top;
}

/**
 * Init dynamic configuration using map logic and specific configuration
 * @param cfg config file
 */
void
init_dynamic_config (struct rspamd_config *cfg)
{
	struct config_json_buf *jb, **pjb;

	if (cfg->dynamic_conf == NULL) {
		/* No dynamic conf has been specified, so do not try to load it */
		return;
	}

	/* Now try to add map with json data */
	jb = g_slice_alloc (sizeof (struct config_json_buf));
	pjb = g_malloc (sizeof (struct config_json_buf *));
	jb->buf = NULL;
	jb->cfg = cfg;
	*pjb = jb;
	cfg->current_dynamic_conf = ucl_object_typed_new (UCL_ARRAY);

	if (!rspamd_map_add (cfg, cfg->dynamic_conf, "Dynamic configuration map",
		json_config_read_cb, json_config_fin_cb, (void **)pjb)) {
		msg_err ("cannot add map for configuration %s", cfg->dynamic_conf);
	}
}

/**
 * Dump dynamic configuration to the disk
 * @param cfg
 * @return
 */
gboolean
dump_dynamic_config (struct rspamd_config *cfg)
{
	struct stat st;
	gchar *dir, pathbuf[PATH_MAX];
	gint fd;

	if (cfg->dynamic_conf == NULL || cfg->current_dynamic_conf == NULL) {
		/* No dynamic conf has been specified, so do not try to dump it */
		msg_err ("cannot save dynamic conf as it is not specified");
		return FALSE;
	}

	dir = g_path_get_dirname (cfg->dynamic_conf);
	if (dir == NULL) {
		msg_err ("invalid path: %s", cfg->dynamic_conf);
		return FALSE;
	}

	if (stat (cfg->dynamic_conf, &st) == -1) {
		msg_debug ("%s is unavailable: %s", cfg->dynamic_conf,
			strerror (errno));
		st.st_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
	}
	if (access (dir, W_OK | R_OK) == -1) {
		msg_warn ("%s is inaccessible: %s", dir, strerror (errno));
		g_free (dir);
		return FALSE;
	}
	rspamd_snprintf (pathbuf,
		sizeof (pathbuf),
		"%s%crconf-XXXXXX",
		dir,
		G_DIR_SEPARATOR);
	g_free (dir);
#ifdef HAVE_MKSTEMP
	/* Umask is set before */
	fd = mkstemp (pathbuf);
#else
	fd = g_mkstemp_full (pathbuf, O_RDWR, S_IWUSR | S_IRUSR);
#endif
	if (fd == -1) {
		msg_err ("mkstemp error: %s", strerror (errno));

		return FALSE;
	}

	if (!ucl_object_emit_full (cfg->current_dynamic_conf, UCL_EMIT_JSON,
			ucl_object_emit_fd_funcs (fd))) {
		msg_err ("cannot emit ucl object: %s", strerror (errno));
		close (fd);
		return FALSE;
	}

	(void)unlink (cfg->dynamic_conf);

	/* Rename old config */
	if (rename (pathbuf, cfg->dynamic_conf) == -1) {
		msg_err ("rename error: %s", strerror (errno));
		close (fd);
		unlink (pathbuf);
		return FALSE;
	}
	/* Set permissions */

	if (chmod (cfg->dynamic_conf, st.st_mode) == -1) {
		msg_warn ("chmod failed: %s", strerror (errno));
	}

	close (fd);
	return TRUE;
}

static ucl_object_t*
new_dynamic_metric (const gchar *metric_name, ucl_object_t *top)
{
	ucl_object_t *metric;

	metric = ucl_object_typed_new (UCL_OBJECT);

	ucl_object_insert_key (metric, ucl_object_fromstring (metric_name),
			"metric", sizeof ("metric") - 1, true);
	ucl_object_insert_key (metric, ucl_object_typed_new (UCL_ARRAY),
			"actions", sizeof ("actions") - 1, false);
	ucl_object_insert_key (metric, ucl_object_typed_new (UCL_ARRAY),
			"symbols", sizeof ("symbols") - 1, false);

	ucl_array_append (top, metric);

	return metric;
}

static ucl_object_t *
dynamic_metric_find_elt (const ucl_object_t *arr, const gchar *name)
{
	ucl_object_iter_t it = NULL;
	const ucl_object_t *cur, *n;

	while ((cur = ucl_iterate_object (arr, &it, true)) != NULL) {
		if (cur->type == UCL_OBJECT) {
			n = ucl_object_find_key (cur, "name");
			if (n && n->type == UCL_STRING &&
				strcmp (name, ucl_object_tostring (n)) == 0) {
				return (ucl_object_t *)ucl_object_find_key (cur, "value");
			}
		}
	}

	return NULL;
}

static ucl_object_t *
dynamic_metric_find_metric (const ucl_object_t *arr, const gchar *metric)
{
	ucl_object_iter_t it = NULL;
	const ucl_object_t *cur, *n;

	while ((cur = ucl_iterate_object (arr, &it, true)) != NULL) {
		if (cur->type == UCL_OBJECT) {
			n = ucl_object_find_key (cur, "metric");
			if (n && n->type == UCL_STRING &&
				strcmp (metric, ucl_object_tostring (n)) == 0) {
				return (ucl_object_t *)cur;
			}
		}
	}

	return NULL;
}

static ucl_object_t *
new_dynamic_elt (ucl_object_t *arr, const gchar *name, gdouble value)
{
	ucl_object_t *n;

	n = ucl_object_typed_new (UCL_OBJECT);
	ucl_object_insert_key (n, ucl_object_fromstring (name), "name",
		sizeof ("name") - 1, false);
	ucl_object_insert_key (n, ucl_object_fromdouble (value), "value",
		sizeof ("value") - 1, false);

	ucl_array_append (arr, n);

	return n;
}

/**
 * Add symbol for specified metric
 * @param cfg config file object
 * @param metric metric's name
 * @param symbol symbol's name
 * @param value value of symbol
 * @return
 */
gboolean
add_dynamic_symbol (struct rspamd_config *cfg,
	const gchar *metric_name,
	const gchar *symbol,
	gdouble value)
{
	ucl_object_t *metric, *syms;

	if (cfg->dynamic_conf == NULL) {
		msg_info ("dynamic conf is disabled");
		return FALSE;
	}

	metric = dynamic_metric_find_metric (cfg->current_dynamic_conf,
			metric_name);
	if (metric == NULL) {
		metric = new_dynamic_metric (metric_name, cfg->current_dynamic_conf);
	}

	syms = (ucl_object_t *)ucl_object_find_key (metric, "symbols");
	if (syms != NULL) {
		ucl_object_t *sym;

		sym = dynamic_metric_find_elt (syms, symbol);
		if (sym) {
			sym->value.dv = value;
		}
		else {
			new_dynamic_elt (syms, symbol, value);
		}
	}

	apply_dynamic_conf (cfg->current_dynamic_conf, cfg);

	return TRUE;
}


/**
 * Add action for specified metric
 * @param cfg config file object
 * @param metric metric's name
 * @param action action's name
 * @param value value of symbol
 * @return
 */
gboolean
add_dynamic_action (struct rspamd_config *cfg,
	const gchar *metric_name,
	guint action,
	gdouble value)
{
	ucl_object_t *metric, *acts;
	const gchar *action_name = rspamd_action_to_str (action);

	if (cfg->dynamic_conf == NULL) {
		msg_info ("dynamic conf is disabled");
		return FALSE;
	}

	metric = dynamic_metric_find_metric (cfg->current_dynamic_conf,
			metric_name);
	if (metric == NULL) {
		metric = new_dynamic_metric (metric_name, cfg->current_dynamic_conf);
	}

	acts = (ucl_object_t *)ucl_object_find_key (metric, "actions");
	if (acts != NULL) {
		ucl_object_t *act;

		act = dynamic_metric_find_elt (acts, action_name);
		if (act) {
			act->value.dv = value;
		}
		else {
			new_dynamic_elt (acts, action_name, value);
		}
	}

	apply_dynamic_conf (cfg->current_dynamic_conf, cfg);

	return TRUE;
}
