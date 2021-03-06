#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <float.h>
#include <maxminddb.h>
#include "vmod_geo.h"

#ifndef DEBUG
#define DEBUG 0
#endif

//**********************************************************************
// NOTICE!! All functions that return pointers use calloc so you must
// free the return values.
//**********************************************************************

static char*  MMDB_CITY_PATH = MAX_CITY_DB;
static char*  DEFAULT_WEATHER_CODE = "New YorkNYUS";

// close gets called by varnish when then the treads destroyed
void close_mmdb(void *mmdb_handle)
{
    // don't do anything if the db didn't open correctly.
    if (mmdb_handle == NULL) {
        return;
    }
    MMDB_s *handle = (MMDB_s *)mmdb_handle;
    MMDB_close(handle);
}

// Open the maxmind db file
int
open_mmdb(MMDB_s *mmdb_handle)
{
    int mmdb_baddb = MMDB_open(MMDB_CITY_PATH, MMDB_MODE_MMAP, mmdb_handle);
    if (mmdb_baddb != MMDB_SUCCESS) {
#if DEBUG
        fprintf(stderr, "[ERROR] open_mmdb: Can't open %s - %s\n",
            MMDB_CITY_PATH, MMDB_strerror(mmdb_baddb));
        if (MMDB_IO_ERROR == mmdb_baddb) {
            fprintf(stderr,
                "[ERROR] open_mmdb: IO error: %s\n",
                strerror(mmdb_baddb));
        }
#endif
        mmdb_handle = NULL;
        return 1;
    }
    return 0;
}

// lookup an ip address using the maxmind db and return the value
// lookup_path described in this doc: http://maxmind.github.io/MaxMind-DB/
const char *
geo_lookup(MMDB_s *const mmdb_handle, const char *ipstr, const char **lookup_path)
{
    char *data = NULL;
    // Lookup IP in the DB
    int gai_error, mmdb_error;
    MMDB_lookup_result_s result =
        MMDB_lookup_string(mmdb_handle, ipstr, &gai_error, &mmdb_error);

    if (0 != gai_error) {
#if DEBUG
        fprintf(stderr,
            "[INFO] Error from getaddrinfo for %s - %s\n\n",
            ipstr, gai_strerror(gai_error));
#endif
        return NULL;
    }

    if (MMDB_SUCCESS != mmdb_error) {
#if DEBUG
        fprintf(stderr,
            "[ERROR] Got an error from libmaxminddb: %s\n\n",
            MMDB_strerror(mmdb_error));
#endif
        return NULL;
    }

    // Parse results
    MMDB_entry_data_s entry_data;
    int exit_code = 0;

    if (result.found_entry) {
        int status = MMDB_aget_value(&result.entry, &entry_data, lookup_path);

        if (MMDB_SUCCESS != status) {
#if DEBUG
            fprintf(
                stderr,
                    "[WARN] Got an error looking up the entry data. Make sure \
 the lookup_path is correct. %s\n",
                    MMDB_strerror(status));
#endif
            exit_code = 4;
            return NULL;
        }

        if (entry_data.has_data) {
            switch(entry_data.type) {
            case MMDB_DATA_TYPE_UTF8_STRING:
                data = strndup(entry_data.utf8_string, entry_data.data_size);
                break;
            case MMDB_DATA_TYPE_UINT16: {
                uint16_t num = UINT16_MAX;
                int len      = (int)((ceil(log10(num)))*sizeof(char));
                data         = calloc(sizeof(char), len+1);
                snprintf(data, len, "%u", entry_data.uint16);
                break;
            }
            default:
#if DEBUG
                fprintf(
                    stderr,
                        "[WARN] No handler for entry data type (%d) was found\n",
                        entry_data.type);
#endif
                exit_code = 6;
                break;
            }
        } else {
            return NULL;
        }
    } else {
#if DEBUG
        fprintf(
            stderr,
                "[INFO] No entry for this IP address (%s) was found\n",
                ipstr);
#endif
        exit_code = 5;
        return NULL;
    }
    return data;
}

// Given a valid result and some entry data, lookup a value
// NOTE: You must free() the return value if != NULL
//
// @result - pointer to a result after calling MMDB_lookup_string
// @path - lookup value for MMDB_aget_value
// @return - NULL on failure
char *
get_value(MMDB_lookup_result_s *result, const char **path)
{
    MMDB_entry_data_s entry_data;
    int status  = MMDB_aget_value( &(*result).entry, &entry_data, path);
    if (MMDB_SUCCESS != status) {
#if DEBUG
        fprintf(
            stderr,
                "[WARN] get_value got an error looking up the entry data. Make sure you use the correct path - %s\n",
                MMDB_strerror(status));
#endif
        return NULL;
    }
    char *value = NULL;
    if (entry_data.has_data) {
        switch(entry_data.type) {
        case MMDB_DATA_TYPE_UTF8_STRING:
            value = strndup(entry_data.utf8_string, entry_data.data_size);
            break;
        case MMDB_DATA_TYPE_UINT16: {
            uint16_t num = UINT16_MAX;
            int len      = (int)((ceil(log10(num)))*sizeof(char));
            value        = calloc(sizeof(char), len+1);
            snprintf(value, len, "%u", entry_data.uint16);
            break;
        }
        case MMDB_DATA_TYPE_DOUBLE: {
            double num = DBL_MAX;
            int len    = (int)((ceil(log10(num)))*sizeof(char));
            value      = calloc(sizeof(char), len+1);
            snprintf(value, len, "%f", entry_data.double_value);
            break;
        }
        case MMDB_DATA_TYPE_BOOLEAN:
            // i'm assuming true == 1 and false == 0
            value   = calloc(sizeof(char), 2);
            snprintf(value, 1, "%d", entry_data.boolean);
            break;
        default:
#if DEBUG
            fprintf(
                stderr,
                    "[WARN] get_value: No handler for entry data type (%d) was found. \n",
                    entry_data.type);
#endif
            break;
        }
    }
    return value;
}

// This function builds up a code we need to lookup weather
// using Accuweather data.
// country code                     e.g. US
// city                             e.g. Beverly Hills
// if country code == US, get region e.g. CA
// And then return "Beverly HillsCAUS" if a US address or
//                    "Paris--FR" if non US
char *
geo_lookup_weather(MMDB_s *const mmdb_handle, const char *ipstr, int use_default)
{
    if (mmdb_handle == NULL) {
        fprintf(stderr, "[WARN] geo vmod given NULL maxmind db handle");
        return strdup(DEFAULT_WEATHER_CODE);
    }

    char *data;
    // Lookup IP in the DB
    int ip_lookup_failed, db_status;
    MMDB_lookup_result_s result =
        MMDB_lookup_string(mmdb_handle, ipstr, &ip_lookup_failed, &db_status);

    if (ip_lookup_failed) {
#if DEBUG
        fprintf(stderr,
            "[WARN] vmod_lookup_weathercode: Error from getaddrinfo for IP: %s Error Message: %s\n",
            ipstr, gai_strerror(ip_lookup_failed));
#endif
        // we don't want null, if we're not using default
        if (use_default) {
            return strdup(DEFAULT_WEATHER_CODE);
        } else {
            return strdup("--");
        }
    }

    if (db_status != MMDB_SUCCESS) {
#if DEBUG
        fprintf(stderr,
            "[ERROR] vmod_lookup_weathercode: libmaxminddb failure. \
Maybe there is something wrong with the file: %s libmaxmind error: %s\n",
            MMDB_CITY_PATH,
            MMDB_strerror(db_status));
#endif
        if (use_default) {
            return strdup(DEFAULT_WEATHER_CODE);
        } else {
            return strdup("--");
        }
    }

    // these varaibles will hold our results
    char *country = NULL;
    char *city    = NULL;
    char *state   = NULL;

    // these are used to extract values from the mmdb
    const char *country_lookup[] = {"country", "iso_code", NULL};
    const char *city_lookup[]    = {"city", "names", "en", NULL};
    const char *state_lookup[]   = {"subdivisions", "0", "iso_code", NULL};

    if (result.found_entry) {
        country = get_value(&result, country_lookup);
        city    = get_value(&result, city_lookup);

        if (country != NULL && strcmp(country,"US") == 0) {
            state = get_value(&result, state_lookup);
        } else {
            state = strdup("--");
        }

        // we should always return new york
        if (country == NULL || city == NULL || state == NULL) {

            if (use_default) {
                data = strdup(DEFAULT_WEATHER_CODE);
            } else {
                if (country == NULL) {
                    country = strdup("--");
                }
                if (city == NULL) {
                    city = strdup("--");
                }
                if (state == NULL) {
                    state = strdup("--");
                }
                size_t chars = (sizeof(char) * (strlen(country) + strlen(city) + strlen(state)) ) + 1;
                data = malloc(chars);
                sprintf(data, "%s%s%s", city, state, country);
            }
        } else {
            size_t chars = (sizeof(char)* ( strlen(country) + strlen(city) + strlen(state)) ) + 1;
            data = malloc(chars);
            sprintf(data, "%s%s%s", city, state, country);
        }

    } else {
#if DEBUG
        fprintf(
            stderr,
                "[INFO] No entry for this IP address (%s) was found\n",
                ipstr);
#endif
        data = strdup(DEFAULT_WEATHER_CODE);
    }
    
    if (country != NULL) {
        free(country);
    }

    if (city != NULL) {
        free(city);
    }

    if (state != NULL) {
        free(state);
    }

    return data;
}

// a utility function for doing large scale testing
void
dump_failed_lookup(MMDB_s *const mmdb_handle, const char *ipstr, const char *outputfile)
{
    if (mmdb_handle == NULL) {
        fprintf(stderr, "[WARN] geo vmod given NULL maxmind db handle");
        return;
    }

    // Lookup IP in the DB
    int ip_lookup_failed, db_status;
    MMDB_lookup_result_s result =
        MMDB_lookup_string(mmdb_handle, ipstr, &ip_lookup_failed, &db_status);

    if (ip_lookup_failed) {
#if DEBUG
        fprintf(stderr,
            "[WARN] vmod_lookup_weathercode: Error from getaddrinfo for IP: %s Error Message: %s\n",
            ipstr, gai_strerror(ip_lookup_failed));
#endif
        return;
    }

    if (db_status != MMDB_SUCCESS) {
#if DEBUG
        fprintf(stderr,
            "[ERROR] vmod_lookup_weathercode: libmaxminddb failure. \
Maybe there is something wrong with the file: %s libmaxmind error: %s\n",
            MMDB_CITY_PATH,
            MMDB_strerror(db_status));
#endif
        return;
    }

    FILE *f = fopen(outputfile, "a+");

    if (f == NULL) {
#if DEBUG
        fprintf(stderr, "[ERROR] Unable to open the output file %s\n", outputfile);
#endif
        return;
    }

    MMDB_entry_data_list_s *entry_data_list = NULL;
    int status = MMDB_get_entry_data_list(&result.entry,
        &entry_data_list);

    if (MMDB_SUCCESS != status) {
#ifndef DEBUG
        fprintf(
            stderr,
                "Got an error looking up the entry data - %s\n",
                MMDB_strerror(status));
#endif
        return;
    }

    if (entry_data_list != NULL) {
        const char *proxy_lookup[] = {"traits", "is_anonymous_proxy", NULL};
        const char *trait_lookup[] = {"traits", "is_satellite_provider", NULL};
        const char *lat_lookup[] = {"location", "latitude", NULL};
        const char *lon_lookup[] = {"location", "longitude", NULL};
        char *lat = get_value(&result, lat_lookup);
        char *lon = get_value(&result, lon_lookup);
        char *proxy = get_value(&result, proxy_lookup);
        char *satellite = get_value(&result, trait_lookup);
        if ( (proxy != NULL &&
                strcmp(proxy,"true")) ||
            (satellite != NULL && strcmp(satellite,"true"))) {
            // we don't care about this.
            char *proxy_satelitte = (proxy == NULL) ? "satellite" : "proxy";
            fprintf(f,"%s,%s\n",ipstr, proxy_satelitte);
        } else if (lat != NULL && lon != NULL) {
            fprintf(f,"%s,%s,%s\n",ipstr,lat,lon);
        } else {
            // macro this out. If you need to debug and output all that comes back for
            // a given lookup, this can be re-enabled.
#ifdef DONTRUN
            const char *reg_lookup[] = {"registered_country", "iso_code", NULL};
            char *reg_country = get_value(&result, reg_lookup);
            if (reg_country != NULL) {
                fprintf(f, "%s,%s\n", ipstr, reg_country);
            } else {
                size_t chars = (sizeof(char) * strlen(ipstr)+50);
                stuff = calloc( chars, sizeof(char) );
                fprintf(f, "{\"%s\":", ipstr);
                MMDB_dump_entry_data_list(f, entry_data_list, 2);
                fprintf(f, "},");
            }
#endif
        }
    }
    fclose(f);
}

// we only want the first part of the cookie, up to the |
char *
get_weather_code_from_cookie(const char *cookiestr, const char *cookiename)
{
    char* found = get_cookie(cookiestr, cookiename);
    if (found != NULL) {
        char* sep = strstr(found, "|");
        if (sep != NULL) {
            *sep = '\0';
        }
    }
    return found;
}


char *
get_cookie(const char *cookiestr, const char *cookiename)
{
    const char *found = cookiestr;

    do {
        found = strstr(found, cookiename);

        if (found == NULL) {
            return NULL;
        }

        found += strlen(cookiename);

        // next character has to be equal or space
        if (*found == ' ' || *found == '=') {
            break;
        }

    } while(found);

    // cookies can have white space after the name, before the =
    while (*found && *found != '=') {
        ++found; // move past the = sign
    }

    // should be at equal at this point
    if (*found != '=') {
        return NULL;
    }
    ++found;

    // we should not have any white space after the = symbol
    // and if the next char is a ; there is no value for the cookie
    if (*found == '\0' || *found == ';' || *found == ' ') {
        return NULL;
    }

    // find the end of the cookie. cookies are name=value;
    char* end = (char *)found;
    while (*end && *end != ';' && *end != ' ') {
        ++end;
    }

    int len      = end - found;
    char* result = NULL;
    result = calloc(sizeof(char), len+1);
    if (!result) {
        return NULL;
    }
    strncpy(result, found, len);
    return result;
}
