// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// obs_stub.cpp — Stub implementations of OBS API functions.
// Compiled ONLY when DANCEHAP_HAVE_OBS is not defined (stub mode).
// Provides functional-enough implementations of obs_data, obs_properties,
// and source-registration so that plugin code compiles and unit tests can
// verify behaviour through the standard OBS API surface.

#include "obs_compat.hpp"

#ifndef DANCEHAP_HAVE_OBS

// ---------------------------------------------------------------------------
// Stub global state (for test verification)
// ---------------------------------------------------------------------------

namespace {

// Registration tracking
int g_registration_count = 0;
const obs_source_info *g_last_source = nullptr;

} // anonymous namespace

// ---------------------------------------------------------------------------
// obs_data stub
// ---------------------------------------------------------------------------

obs_data_t *obs_data_create(void)
{
    return new obs_data();
}

void obs_data_release(obs_data_t *data)
{
    if (!data) return;
    if (--data->ref_count <= 0) {
        delete data;
    }
}

void obs_data_set_default_string(obs_data_t *settings,
                                  const char *name, const char *val)
{
    if (!settings || !name) return;
    settings->default_strings[name] = val ? val : "";
}

void obs_data_set_default_bool(obs_data_t *settings,
                                const char *name, bool val)
{
    if (!settings || !name) return;
    settings->default_bools[name] = val;
}

void obs_data_set_default_int(obs_data_t *settings,
                               const char *name, long long val)
{
    if (!settings || !name) return;
    settings->default_ints[name] = val;
}

const char *obs_data_get_string(obs_data_t *settings, const char *name)
{
    if (!settings || !name) return "";
    // Explicit value takes priority, then default, then empty string.
    auto it = settings->strings.find(name);
    if (it != settings->strings.end()) return it->second.c_str();
    auto dit = settings->default_strings.find(name);
    if (dit != settings->default_strings.end()) return dit->second.c_str();
    return "";
}

bool obs_data_get_bool(obs_data_t *settings, const char *name)
{
    if (!settings || !name) return false;
    auto it = settings->bools.find(name);
    if (it != settings->bools.end()) return it->second;
    auto dit = settings->default_bools.find(name);
    if (dit != settings->default_bools.end()) return dit->second;
    return false;
}

long long obs_data_get_int(obs_data_t *settings, const char *name)
{
    if (!settings || !name) return 0;
    auto it = settings->ints.find(name);
    if (it != settings->ints.end()) return it->second;
    auto dit = settings->default_ints.find(name);
    if (dit != settings->default_ints.end()) return dit->second;
    return 0;
}

// ---------------------------------------------------------------------------
// obs_properties stub
// ---------------------------------------------------------------------------

obs_properties_t *obs_properties_create(void)
{
    return new obs_properties();
}

void obs_properties_destroy(obs_properties_t *props)
{
    delete props;
}

obs_properties_t *obs_properties_add_path(obs_properties_t *props,
                                           const char *name,
                                           const char *description,
                                           enum obs_path_type /*type*/,
                                           const char *filter,
                                           const char * /*default_path*/)
{
    if (!props || !name) return props;
    obs_properties::property p;
    p.name = name;
    p.description = description ? description : "";
    p.kind = "path";
    p.filter = filter ? filter : "";
    props->props.push_back(std::move(p));
    return props;
}

obs_properties_t *obs_properties_add_bool(obs_properties_t *props,
                                           const char *name,
                                           const char *description)
{
    if (!props || !name) return props;
    obs_properties::property p;
    p.name = name;
    p.description = description ? description : "";
    p.kind = "bool";
    props->props.push_back(std::move(p));
    return props;
}

obs_properties_t *obs_properties_add_int(obs_properties_t *props,
                                          const char *name,
                                          const char *description,
                                          int min_val, int max_val, int step_val)
{
    if (!props || !name) return props;
    obs_properties::property p;
    p.name = name;
    p.description = description ? description : "";
    p.kind = "int";
    p.min_val = min_val;
    p.max_val = max_val;
    p.step_val = step_val;
    props->props.push_back(std::move(p));
    return props;
}

// ---------------------------------------------------------------------------
// Source registration stub
// ---------------------------------------------------------------------------

void obs_register_source_s(const struct obs_source_info *info, std::size_t /*size*/)
{
    if (!info) return;
    g_registration_count++;
    g_last_source = info;
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

void obs_stub_reset(void)
{
    g_registration_count = 0;
    g_last_source = nullptr;
}

int obs_stub_registration_count(void)
{
    return g_registration_count;
}

const struct obs_source_info *obs_stub_last_registered_source(void)
{
    return g_last_source;
}

#endif // !DANCEHAP_HAVE_OBS
