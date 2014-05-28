#include <stdlib.h>
#include <maxminddb.h>

#include "vrt.h"
#include "bin/varnishd/cache.h"
#include "include/vct.h"
#include "vcc_if.h"

static char* MMDB_CITY_PATH = "/mnt/mmdb/GeoLite2-City.mmdb";
static char* MMDB_COUNTRY_PATH = "/mnt/mmdb/GeoLite2-Country.mmdb";


int
init_function(struct vmod_priv *priv, const struct VCL_conf *conf)
{
        return 1;
}

const char *
vmod_lookup(struct sess *sp, const char *ipstr, const char *type)
{
        MMDB_s mmdb;
        char *data = NULL;

        // Create DB connection
        int status = MMDB_open(MMDB_CITY_PATH, MMDB_MODE_MMAP, &mmdb);
        if (MMDB_SUCCESS != status) {
                fprintf(stderr, "\n  Can't open %s - %s\n",
                        MMDB_CITY_PATH, MMDB_strerror(status));

                if (MMDB_IO_ERROR == status) {
                    fprintf(stderr, "    IO error: %s\n", strerror(errno));
                }
                exit(1);
        }

        // Lookup IP in the DB
        int gai_error, mmdb_error;
        MMDB_lookup_result_s result =
            MMDB_lookup_string(&mmdb, ipstr, &gai_error, &mmdb_error);

        if (0 != gai_error) {
            fprintf(stderr,
                    "\n  Error from getaddrinfo for %s - %s\n\n",
                    ipstr, gai_strerror(gai_error));
            exit(2);
        }

        if (MMDB_SUCCESS != mmdb_error) {
            fprintf(stderr,
                    "\n  Got an error from libmaxminddb: %s\n\n",
                    MMDB_strerror(mmdb_error));
            exit(3);
        }

        // Parse results
        MMDB_entry_data_s entry_data;
        int exit_code = 0;
        if (result.found_entry) {
            int status = MMDB_get_value(&result.entry, &entry_data, type, "names", "en", NULL);

            if (MMDB_SUCCESS != status) {
                fprintf(
                    stderr,
                    "Got an error looking up the entry data - %s\n",
                    MMDB_strerror(status));
                exit_code = 4;
            }

            if (entry_data.has_data) {
                switch(entry_data.type){
                    case MMDB_DATA_TYPE_UTF8_STRING:
                        data = entry_data.utf8_string;
                        break;
                    default:
                        fprintf(
                            stderr,
                            "\n  No handler for entry data type (%d) was found\n\n",
                            entry_data.type);
                        break;
                }
            }
    } else {
        fprintf(
            stderr,
            "\n  No entry for this IP address (%s) was found\n\n",
            ipstr);
        exit_code = 5;
    }

    char *cp;
    cp = WS_Dup(sp->wrk->ws, data);
    MMDB_close(&mmdb);
    return cp;
}

const char*
vmod_city(struct sess *sp, const char *ipstr)
{
        return vmod_lookup(sp, ipstr, "city");
}

const char*
vmod_country(struct sess *sp, const char *ipstr)
{
        return vmod_lookup(sp, ipstr, "country");
}
