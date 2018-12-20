/*
 * Copyright 2017-2018 Baidu Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "openrasp_security_policy.h"
#include "openrasp_hook.h"
#include "openrasp_ini.h"
#include "agent/shared_config_manager.h"
#include "utils/regex.h"

static void _check_header_content_type_if_html(void *data, void *arg);
static int _detect_param_occur_in_html_output(const char *param);
static bool _gpc_parameter_filter(const zval *param);
static bool _is_content_type_html();

static php_output_handler *openrasp_output_handler_init(const char *handler_name, size_t handler_name_len, size_t chunk_size, int flags);
static void openrasp_clean_output_start(const char *name, size_t name_len);
static int openrasp_output_handler(void **nothing, php_output_context *output_context);

static int openrasp_output_handler(void **nothing, php_output_context *output_context)
{
    int status = FAILURE;
    if (_is_content_type_html() &&
        (output_context->op & PHP_OUTPUT_HANDLER_START) &&
        (output_context->op & PHP_OUTPUT_HANDLER_FINAL))
    {
        status = _detect_param_occur_in_html_output(output_context->in.data);
        if (status == SUCCESS)
        {
            set_location_header();
        }
    }
    return status;
}

static php_output_handler *openrasp_output_handler_init(const char *handler_name, size_t handler_name_len, size_t chunk_size, int flags)
{
    if (chunk_size)
    {
        return nullptr;
    }
    return php_output_handler_create_internal(handler_name, handler_name_len, openrasp_output_handler, chunk_size, flags);
}

static void openrasp_clean_output_start(const char *name, size_t name_len)
{
    php_output_handler *h;

    if (h = openrasp_output_handler_init(name, name_len, 0, PHP_OUTPUT_HANDLER_STDFLAGS))
    {
        php_output_handler_start(h);
    }
}

static void _check_header_content_type_if_html(void *data, void *arg)
{
    bool *is_html = static_cast<bool *>(arg);
    if (*is_html)
    {
        sapi_header_struct *sapi_header = (sapi_header_struct *)data;
        static const char *suffix = "Content-type";
        char *header = (char *)(sapi_header->header);
        size_t header_len = strlen(header);
        size_t suffix_len = strlen(suffix);
        if (header_len > suffix_len &&
            strncmp(suffix, header, suffix_len) == 0 &&
            NULL == strstr(header, "text/html"))
        {
            *is_html = false;
        }
    }
}

static bool _gpc_parameter_filter(const zval *param)
{
    if (Z_TYPE_P(param) == IS_STRING && Z_STRLEN_P(param) > OPENRASP_CONFIG(xss.min_param_length))
    {
        if (openrasp::regex_match(Z_STRVAL_P(param), OPENRASP_CONFIG(xss.filter_regex).c_str()))
        {
            return true;
        }
    }
    return false;
}

static int _detect_param_occur_in_html_output(const char *param)
{
    int status = FAILURE;
    if (Z_TYPE(PG(http_globals)[TRACK_VARS_GET]) != IS_ARRAY &&
        !zend_is_auto_global_str(ZEND_STRL("_GET")))
    {
        return FAILURE;
    }
    zval *global = &PG(http_globals)[TRACK_VARS_GET];
    int count = 0;
    OpenRASPActionType action = openrasp::scm->get_buildin_check_action(XSS);
    zval *val;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(global), val)
    {
        if (_gpc_parameter_filter(val))
        {
            if (++count > OPENRASP_CONFIG(xss.max_detection_num))
            {
                zval attack_params;
                array_init(&attack_params);
                add_assoc_long(&attack_params, "count", count);
                zval plugin_message;
                ZVAL_STRING(&plugin_message, _("Excessively suspected xss parameters"));
                openrasp_buildin_php_risk_handle(action, XSS, 100, &attack_params, &plugin_message);
                return SUCCESS;
            }
            if (NULL != strstr(param, Z_STRVAL_P(val)))
            {
                zval attack_params;
                array_init(&attack_params);
                add_assoc_string(&attack_params, "parameter", Z_STRVAL_P(val));
                zval plugin_message;
                ZVAL_STR(&plugin_message, strpprintf(0, _("Reflected XSS attack detected: using get parameter: '%s'"), Z_STRVAL_P(val)));
                openrasp_buildin_php_risk_handle(action, XSS, 100, &attack_params, &plugin_message);
                return SUCCESS;
            }
        }
    }
    ZEND_HASH_FOREACH_END();
    return status;
}

static bool _is_content_type_html()
{
    bool is_html = true;
    zend_llist_apply_with_argument(&SG(sapi_headers).headers, _check_header_content_type_if_html, &is_html);
    return is_html;
}

PHP_MINIT_FUNCTION(openrasp_output_detect)
{
    php_output_handler_alias_register(ZEND_STRL("openrasp_ob_handler"), openrasp_output_handler_init);
    return SUCCESS;
}

PHP_RINIT_FUNCTION(openrasp_output_detect)
{
    if (!openrasp_check_type_ignored(XSS))
    {
        openrasp_clean_output_start(ZEND_STRL("openrasp_ob_handler"));
    }
    return SUCCESS;
}