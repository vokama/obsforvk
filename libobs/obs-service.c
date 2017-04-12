/******************************************************************************
Copyright (C) 2014 by Hugh Bailey <obs.jim@gmail.com>

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
******************************************************************************/
#include <curl/curl.h>
#include <jansson.h>
#include "obs-internal.h"

struct vk_common {
	char *service;
	char *server;
	char *key;
};

static void vk_log_error(const char *prefix,
	const char *error, const char *error_description)
{
	char errorlog_path[512];

	os_get_config_path(errorlog_path, sizeof(errorlog_path),
		"vk-games/vk_logs");
	if (!os_file_exists(errorlog_path))
		os_mkdir(errorlog_path);
	strcat(errorlog_path, "/errors.txt");
	FILE *errorlog = os_fopen(errorlog_path, "ab");
	fprintf(errorlog, "%s: %s %s\n", prefix,
		error, error_description);
	fclose(errorlog);
}

static void vk_log_json_error(const char *prefix, const json_t *json)
{
	json_t *error, *error_description;

	if (error = json_object_get(json, "error")) {
		error_description = json_object_get(json, "error_description");
		vk_log_error(prefix, json_string_value(error),
			json_string_value(error_description));
	}
}

int get_loginstatus(json_t *root)
{
	const char *error_str, *error_description_str, *validation_type_str;
	if (!root)
		return not_json;

	json_t *error = json_object_get(root, "error");
	json_t *error_description = json_object_get(root,
		"error_description");
	json_t *validation_type = json_object_get(root,
		"validation_type");

	if (!error)
		return ok;
	error_str = json_string_value(error);
	error_description_str =
		error_description ? json_string_value(error_description) : "";
	validation_type_str =
		validation_type ? json_string_value(validation_type) : "";
	vk_log_error("Login error", error_str, error_description_str);

	if (!strcmp(error_str, "invalid_client"))
		return invalid_client;
	if (!strcmp(error_str, "need_captcha"))
		return need_captcha;

	if (!strcmp(validation_type_str, "2fa_app"))
		return need_appcode;
	if (!strcmp(validation_type_str, "2fa_sms"))
		return need_smscode;

	if (!strcmp(error_description_str, "wrong code"))
		return wrong_code;
	if (!strcmp(error_description_str,
		"please open redirect_uri in browser"))
		return open_redirect_uri;

	return unknown_error;
}

char *urlenc_and_combine_params(CURL *curl, unsigned nmemb, va_list list)
{
	query_param_t param;
	char *value_enc;
	size_t length = 0;
	char *result = malloc(1);
	*result = '\0';

	for (unsigned i = 0; i < nmemb; i++) {
		param = va_arg(list, query_param_t);
		value_enc = curl_easy_escape(curl, param.value, 0);
		result = realloc(result, 
			(length += strlen(param.name) + strlen(value_enc) + 2)
			+ 1);
		strcat(result, "&");
		strcat(result, param.name);
		strcat(result, "=");
		strcat(result, value_enc);
		curl_free(value_enc);
	}

	return result;
}

size_t write_query_data(char *buffer, size_t size, size_t nmemb, void *userdata)
{
	size_t realsize = size * nmemb;
	struct MemoryStruct *mem = userdata;
	mem->memory = realloc(mem->memory, mem->size + realsize + 1);

	memcpy(mem->memory + mem->size, buffer, realsize);
	mem->size += realsize;
	mem->memory[mem->size] = '\0';

	return realsize;
}

char *auth_query(unsigned nmemb, ...)
{
	CURL *curl;
	CURLcode code;
	long response_code;
	size_t url_length;
	char *url, *params, errorbuf[CURL_ERROR_SIZE];
	const char *url_template = auth_url;
	struct MemoryStruct chunk = {0};
	va_list list;

	curl = curl_easy_init();
	if (curl) {
		va_start(list, nmemb);
		params = urlenc_and_combine_params(curl, nmemb, list);
		va_end(list);
		url_length = strlen(url_template) + strlen(params) - 2;
		url = malloc(url_length + 1);
		sprintf(url, url_template, params);
		free(params);

		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_query_data);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
		/*curl_easy_setopt(curl, CURLOPT_SSLVERSION,
			CURL_SSLVERSION_TLSv1_1);
		try in case SEC_E_BUFFER_TOO_SMALL keeps appearing*/
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorbuf);
		curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 0);
		code = curl_easy_perform(curl);

		if (code != CURLE_OK)
			vk_log_error("CURL error (auth)", errorbuf, "");
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		if (response_code >= 400) {
			char response_code_str[4];
			sprintf(response_code_str, "%ld", response_code);
			vk_log_error("HTTP Error (auth)",
				response_code_str, "");
		}

		curl_easy_cleanup(curl);

		free(url);
	}

	return chunk.memory;  //don't forget to free() it somewhere outside
}

char *api_query(const char *method, unsigned nmemb, ...)
{
	CURL *curl;
	CURLcode code;
	long response_code;
	size_t url_length;
	char *url, *params, errorbuf[CURL_ERROR_SIZE];
	const char *url_template = "https://api.vk.com/method/%s?v=5.53%s";
	struct MemoryStruct chunk = {0};
	va_list list;

	curl = curl_easy_init();
	if (curl) {
		va_start(list, nmemb);
		params = urlenc_and_combine_params(curl, nmemb, list);
		va_end(list);
		url_length = strlen(url_template)
			+ strlen(method) + strlen(params) - 4;
		url = malloc(url_length + 1);
		sprintf(url, url_template, method, params);
		free(params);

		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_query_data);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorbuf);
		curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 0);
		code = curl_easy_perform(curl);

		if (code != CURLE_OK)
			vk_log_error("CURL error (API)", errorbuf, "");
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		if (response_code >= 400) {
			char response_code_str[4];
			sprintf(response_code_str, "%ld", response_code);
			vk_log_error("HTTP Error (API)", response_code_str, "");
		}

		curl_easy_cleanup(curl);

		free(url);
	}

	return chunk.memory;
}

size_t get_captchaimg(const char *url, char **img_p)
{
	CURL *curl;
	CURLcode code;
	long response_code;
	struct MemoryStruct chunk = {0};
	char errorbuf[CURL_ERROR_SIZE];

	curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_query_data);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorbuf);
		curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 0);
		code = curl_easy_perform(curl);

		if (code != CURLE_OK)
			vk_log_error("CURL error (get_captchaimg)",
				errorbuf, "");
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
		if (response_code >= 400) {
			char response_code_str[4];
			sprintf(response_code_str, "%ld", response_code);
			vk_log_error("HTTP Error (get_captchaimg)",
				response_code_str, "");
		}

		curl_easy_cleanup(curl);
	}
	*img_p = chunk.memory;
	return chunk.size;
}

json_t *simple_login(login_data_t *login)
{
	char *response = auth_query(2, login->username, login->password);
	json_t *root = json_loads(response, 0, NULL);
	free(response);
	return root;
}

json_t *login_w_captcha(login_data_t *login, captcha_data_t *captcha)
{
	char *response = auth_query(4,
		login->username, login->password, captcha->sid, captcha->key);
	json_t *root = json_loads(response, 0, NULL);
	free(response);
	return root;
}

json_t *login_w_code(login_data_t *login, query_param_t code)
{
	char *response = auth_query(3, login->username, login->password, code);
	json_t *root = json_loads(response, 0, NULL);
	free(response);
	return root;
}

void get_userdata(const char *access_token_str, char **fullname_p)
{
	char *response;
	const char *first_name_str, *last_name_str;
	json_t *root, *user_info;
	query_param_t access_token = {"access_token"};

	access_token.value = strdup(access_token_str);
	response = api_query("users.get", 1, access_token);
	root = json_loads(response, 0, NULL);
	free(response);
	free(access_token.value);
	user_info = json_array_get(json_object_get(root, "response"), 0);
	first_name_str = json_string_value(
		json_object_get(user_info, "first_name"));
	last_name_str = json_string_value(
		json_object_get(user_info, "last_name"));
	if (first_name_str && last_name_str) {
		*fullname_p = malloc(
			strlen(first_name_str) + strlen(last_name_str) + 2);
		sprintf(*fullname_p, "%s %s", first_name_str, last_name_str);
	} else {
		*fullname_p = NULL;
	}
	json_decref(root);
}

long long get_groupsdata(const char *access_token_str,
	char ***groups_names_p, char ***groups_ids_p)
{
	long long id, radix, i, count, id_length;
	char *response;
	const char *name;
	json_t *root, *groups;
	query_param_t filter = {"filter"}, extended = {"extended"},
		access_token = {"access_token"};

	filter.value = strdup("admin");
	extended.value = strdup("1");
	access_token.value = strdup(access_token_str);
	response = api_query("groups.get", 3, filter, extended, access_token);
	root = json_loads(response, 0, NULL);
	free(response);
	free(filter.value);
	free(extended.value);
	free(access_token.value);
	groups = json_object_get(json_object_get(root, "response"), "items");
	count = json_integer_value(
		json_object_get(json_object_get(root, "response"), "count"));
	*groups_names_p = malloc(count * sizeof(**groups_names_p));
	*groups_ids_p = malloc(count * sizeof(**groups_ids_p));
	for (i = 0; i < count; i++) {
		name = json_string_value(
			json_object_get(json_array_get(groups, i), "name"));
		(*groups_names_p)[i] = malloc(strlen(name) + 1);
		strcpy((*groups_names_p)[i], name);
		id = json_integer_value(
			json_object_get(json_array_get(groups, i), "id"));
		radix = 1;
		for(id_length = 0; id >= radix; id_length++)
			radix *= 10;
		(*groups_ids_p)[i] = malloc(id_length + 1);
		sprintf((*groups_ids_p)[i], "%lld", id);
	}
leave:
	json_decref(root);
	return count;
}

static struct subcategory json_to_subcategory(json_t *json)
{
	struct subcategory subcategory;

	subcategory.id = json_integer_value(json_object_get(json, "id"));
	subcategory.name = strdup(
		json_string_value(json_object_get(json, "label")));

	return subcategory;
}

static subcategories_arr_t json_arr_to_subcategories(json_t *array)
{
	subcategories_arr_t subcategories;
	size_t i;
	json_t *value;

	subcategories.size = json_array_size(array);
	subcategories.array =
		malloc(subcategories.size * sizeof(*subcategories.array));
	json_array_foreach(array, i, value)
		subcategories.array[i] = json_to_subcategory(value);
	
	return subcategories;
}

static struct category json_to_category(json_t *json)
{
	struct category category;

	category.id = json_integer_value(json_object_get(json, "id"));
	category.name = strdup(
		json_string_value(json_object_get(json, "label")));
	category.subcategories =
		json_arr_to_subcategories(json_object_get(json, "sublist"));

	return category;
}

static categories_arr_t *json_arr_to_categories(json_t *array)
{
	categories_arr_t *categories = malloc(sizeof(categories_arr_t));
	size_t i;
	json_t *value;

	categories->size = json_array_size(array);
	categories->array =
		malloc(categories->size * sizeof(*categories->array));

	json_array_foreach(array, i, value)
		categories->array[i] = json_to_category(value);

	return categories;
}

categories_arr_t *get_categories(const char *access_token_str)
{
	char *response;
	query_param_t access_token = {"access_token", strdup(access_token_str)};
	json_t *root, *array;

	response = api_query("video.liveGetCategories", 1, access_token);
	free(access_token.value);
	root = json_loads(response, 0, NULL);
	free(response);

	if (!root)
		return NULL;
	if (!(array = json_object_get(root, "response"))) {
		vk_log_json_error("Unable to get categories", root);
		json_decref(root);
		return NULL;
	}

	return json_arr_to_categories(array);
}

long long init_vk_stream(obs_service_t *service,
	const char *output_name, const char *output_id,
	const char *access_token_str)
{
	char *response;
	long long error_code_int = 0;
	const char *url_str, *key_str, *name_fmt = "Live: %s";
	json_t *root, *stream, *url, *key;
	query_param_t access_token = {"access_token"}, name = {"name"},
		group_id = {"group_id"}, wallpost = {"wallpost"};

	bfree(((struct vk_common*)service->context.data)->server);
	bfree(((struct vk_common*)service->context.data)->key);

	access_token.value = strdup(access_token_str);
	name.value = malloc(strlen(name_fmt) - 2 + strlen(output_name) + 1);
	sprintf(name.value, name_fmt, output_name);
	group_id.value = strdup(output_id);
	wallpost.value = strdup("1");

	response = api_query("video.startStreaming", 4, access_token,
		name, group_id, wallpost);
	root = json_loads(response, 0, NULL);
	free(response);
	free(access_token.value);
	free(name.value);
	free(group_id.value);
	free(wallpost.value);
	stream = json_object_get(json_object_get(root, "response"), "stream");
	if (!stream) {
		((struct vk_common*)service->context.data)->server = bzalloc(1);
		strcpy(((struct vk_common*)service->context.data)->server, "");
		((struct vk_common*)service->context.data)->key = bzalloc(1);
		strcpy(((struct vk_common*)service->context.data)->key, "");
	} else {
		url = json_object_get(stream, "url");
		key = json_object_get(stream, "key");
		url_str = json_string_value(url);
		key_str = json_string_value(key);
		((struct vk_common*)service->context.data)->server = bzalloc(strlen(url_str) + 1);
		strcpy(((struct vk_common*)service->context.data)->server, url_str);
		((struct vk_common*)service->context.data)->key = bzalloc(strlen(key_str) + 1);
		strcpy(((struct vk_common*)service->context.data)->key, key_str);
	}
	json_t *error = json_object_get(root, "error");
	if (error) {
		json_t *error_code = json_object_get(error, "error_code");
		error_code_int = json_integer_value(error_code);
		vk_log_error("Stream Start error",
			json_string_value(json_object_get(error, "error_msg")), "");
	}
	json_decref(root);

	return error_code_int;
}

void stop_vk_stream(const char *output_id, const char *access_token_str)
{
	char *response;
	query_param_t group_id = {"group_id"}, access_token = {"access_token"};

	access_token.value = strdup(access_token_str);
	group_id.value = strdup(output_id);

	response = api_query("video.stopStreaming", 2, access_token, group_id);
	free(response);
	free(group_id.value);
	free(access_token.value);
}

const struct obs_service_info *find_service(const char *id)
{
	size_t i;
	for (i = 0; i < obs->service_types.num; i++)
		if (strcmp(obs->service_types.array[i].id, id) == 0)
			return obs->service_types.array + i;

	return NULL;
}

const char *obs_service_get_display_name(const char *id)
{
	const struct obs_service_info *info = find_service(id);
	return (info != NULL) ? info->get_name(info->type_data) : NULL;
}

static obs_service_t *obs_service_create_internal(const char *id,
	const char *name, obs_data_t *settings, obs_data_t *hotkey_data,
	bool private)
{
	const struct obs_service_info *info = find_service(id);
	struct obs_service *service;

	if (!info) {
		blog(LOG_ERROR, "Service '%s' not found", id);
		return NULL;
	}

	service = bzalloc(sizeof(struct obs_service));

	if (!obs_context_data_init(&service->context, OBS_OBJ_TYPE_SERVICE,
		settings, name, hotkey_data, private)) {
		bfree(service);
		return NULL;
	}

	service->info = *info;
	service->context.data = service->info.create(
		service->context.settings, service);
	if (!service->context.data)
		blog(LOG_ERROR, "Failed to create service '%s'!", name);

	service->control = bzalloc(sizeof(obs_weak_service_t));
	service->control->service = service;

	obs_context_data_insert(&service->context,
		&obs->data.services_mutex,
		&obs->data.first_service);

	blog(LOG_DEBUG, "service '%s' (%s) created", name, id);
	return service;
}

obs_service_t *obs_service_create(const char *id,
	const char *name, obs_data_t *settings, obs_data_t *hotkey_data)
{
	return obs_service_create_internal(id, name, settings, hotkey_data,
		false);
}

obs_service_t *obs_service_create_private(const char *id,
	const char *name, obs_data_t *settings)
{
	return obs_service_create_internal(id, name, settings, NULL, true);
}

static void actually_destroy_service(struct obs_service *service)
{
	if (service->context.data)
		service->info.destroy(service->context.data);

	if (service->output)
		service->output->service = NULL;

	blog(LOG_DEBUG, "service '%s' destroyed", service->context.name);

	obs_context_data_free(&service->context);
	if (service->owns_info_id)
		bfree((void*)service->info.id);
	bfree(service);
}

void obs_service_destroy(obs_service_t *service)
{
	if (service) {
		obs_context_data_remove(&service->context);

		service->destroy = true;

		/* do NOT destroy the service until the service is no
		* longer in use */
		if (!service->active)
			actually_destroy_service(service);
	}
}

const char *obs_service_get_name(const obs_service_t *service)
{
	return obs_service_valid(service, "obs_service_get_name") ?
		service->context.name : NULL;
}

static inline obs_data_t *get_defaults(const struct obs_service_info *info)
{
	obs_data_t *settings = obs_data_create();
	if (info->get_defaults)
		info->get_defaults(settings);
	return settings;
}

obs_data_t *obs_service_defaults(const char *id)
{
	const struct obs_service_info *info = find_service(id);
	return (info) ? get_defaults(info) : NULL;
}

obs_properties_t *obs_get_service_properties(const char *id)
{
	const struct obs_service_info *info = find_service(id);
	if (info && info->get_properties) {
		obs_data_t       *defaults = get_defaults(info);
		obs_properties_t *properties;

		properties = info->get_properties(NULL);
		obs_properties_apply_settings(properties, defaults);
		obs_data_release(defaults);
		return properties;
	}
	return NULL;
}

obs_properties_t *obs_service_properties(const obs_service_t *service)
{
	if (!obs_service_valid(service, "obs_service_properties"))
		return NULL;

	if (service->info.get_properties) {
		obs_properties_t *props;
		props = service->info.get_properties(service->context.data);
		obs_properties_apply_settings(props, service->context.settings);
		return props;
	}

	return NULL;
}

const char *obs_service_get_type(const obs_service_t *service)
{
	return obs_service_valid(service, "obs_service_get_type") ?
		service->info.id : NULL;
}

void obs_service_update(obs_service_t *service, obs_data_t *settings)
{
	if (!obs_service_valid(service, "obs_service_update"))
		return;

	obs_data_apply(service->context.settings, settings);

	if (service->info.update)
		service->info.update(service->context.data,
			service->context.settings);
}

obs_data_t *obs_service_get_settings(const obs_service_t *service)
{
	if (!obs_service_valid(service, "obs_service_get_settings"))
		return NULL;

	obs_data_addref(service->context.settings);
	return service->context.settings;
}

signal_handler_t *obs_service_get_signal_handler(const obs_service_t *service)
{
	return obs_service_valid(service, "obs_service_get_signal_handler") ?
		service->context.signals : NULL;
}

proc_handler_t *obs_service_get_proc_handler(const obs_service_t *service)
{
	return obs_service_valid(service, "obs_service_get_proc_handler") ?
		service->context.procs : NULL;
}

const char *obs_service_get_url(const obs_service_t *service)
{
	if (!obs_service_valid(service, "obs_service_get_url"))
		return NULL;

	if (!service->info.get_url) return NULL;
	return service->info.get_url(service->context.data);
}

const char *obs_service_get_key(const obs_service_t *service)
{
	if (!obs_service_valid(service, "obs_service_get_key"))
		return NULL;

	if (!service->info.get_key) return NULL;
	return service->info.get_key(service->context.data);
}

const char *obs_service_get_username(const obs_service_t *service)
{
	if (!obs_service_valid(service, "obs_service_get_username"))
		return NULL;

	if (!service->info.get_username) return NULL;
	return service->info.get_username(service->context.data);
}

const char *obs_service_get_password(const obs_service_t *service)
{
	if (!obs_service_valid(service, "obs_service_get_password"))
		return NULL;

	if (!service->info.get_password) return NULL;
	return service->info.get_password(service->context.data);
}

void obs_service_activate(struct obs_service *service)
{
	if (!obs_service_valid(service, "obs_service_activate"))
		return;
	if (!service->output) {
		blog(LOG_WARNING, "obs_service_deactivate: service '%s' "
			"is not assigned to an output",
			obs_service_get_name(service));
		return;
	}
	if (service->active)
		return;

	if (service->info.activate)
		service->info.activate(service->context.data,
			service->context.settings);
	service->active = true;
}

void obs_service_deactivate(struct obs_service *service, bool remove)
{
	if (!obs_service_valid(service, "obs_service_deactivate"))
		return;
	if (!service->output) {
		blog(LOG_WARNING, "obs_service_deactivate: service '%s' "
			"is not assigned to an output",
			obs_service_get_name(service));
		return;
	}

	if (!service->active) return;

	if (service->info.deactivate)
		service->info.deactivate(service->context.data);
	service->active = false;

	if (service->destroy)
		actually_destroy_service(service);
	else if (remove)
		service->output = NULL;
}

bool obs_service_initialize(struct obs_service *service,
	struct obs_output *output)
{
	if (!obs_service_valid(service, "obs_service_initialize"))
		return false;
	if (!obs_output_valid(output, "obs_service_initialize"))
		return false;

	if (service->info.initialize)
		return service->info.initialize(service->context.data, output);
	return true;
}

void obs_service_apply_encoder_settings(obs_service_t *service,
	obs_data_t *video_encoder_settings,
	obs_data_t *audio_encoder_settings)
{
	if (!obs_service_valid(service, "obs_service_apply_encoder_settings"))
		return;
	if (!service->info.apply_encoder_settings)
		return;

	if (video_encoder_settings || audio_encoder_settings)
		service->info.apply_encoder_settings(service->context.data,
			video_encoder_settings, audio_encoder_settings);
}

void obs_service_addref(obs_service_t *service)
{
	if (!service)
		return;

	obs_ref_addref(&service->control->ref);
}

void obs_service_release(obs_service_t *service)
{
	if (!service)
		return;

	obs_weak_service_t *control = service->control;
	if (obs_ref_release(&control->ref)) {
		// The order of operations is important here since
		// get_context_by_name in obs.c relies on weak refs
		// being alive while the context is listed
		obs_service_destroy(service);
		obs_weak_service_release(control);
	}
}

void obs_weak_service_addref(obs_weak_service_t *weak)
{
	if (!weak)
		return;

	obs_weak_ref_addref(&weak->ref);
}

void obs_weak_service_release(obs_weak_service_t *weak)
{
	if (!weak)
		return;

	if (obs_weak_ref_release(&weak->ref))
		bfree(weak);
}

obs_service_t *obs_service_get_ref(obs_service_t *service)
{
	if (!service)
		return NULL;

	return obs_weak_service_get_service(service->control);
}

obs_weak_service_t *obs_service_get_weak_service(obs_service_t *service)
{
	if (!service)
		return NULL;

	obs_weak_service_t *weak = service->control;
	obs_weak_service_addref(weak);
	return weak;
}

obs_service_t *obs_weak_service_get_service(obs_weak_service_t *weak)
{
	if (!weak)
		return NULL;

	if (obs_weak_ref_get_ref(&weak->ref))
		return weak->service;

	return NULL;
}

bool obs_weak_service_references_service(obs_weak_service_t *weak,
	obs_service_t *service)
{
	return weak && service && weak->service == service;
}

void *obs_service_get_type_data(obs_service_t *service)
{
	return obs_service_valid(service, "obs_service_get_type_data")
		? service->info.type_data : NULL;
}

const char *obs_service_get_id(const obs_service_t *service)
{
	return obs_service_valid(service, "obs_service_get_id")
		? service->info.id : NULL;
}
