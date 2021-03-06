/* NTP CLI commands
 *
 * Copyright (C) 1997, 98 Kunihiro Ishiguro
 * Copyright (C) 2016 Hewlett Packard Enterprise Development LP
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * File: ntp_vty.c
 *
 * Purpose: To add ntp CLI configuration and display commands
 */

#include <sys/wait.h>
#include <arpa/inet.h>
#include <ctype.h>
#include "vtysh/command.h"
#include "memory.h"
#include "vtysh/vtysh.h"
#include "vtysh/vtysh_user.h"
#include "vswitch-idl.h"
#include "ovsdb-idl.h"
#include "ntp_vty.h"
#include "smap.h"
#include "openvswitch/vlog.h"
#include "openswitch-idl.h"
#include "ovsdb-data.h"
#include "vtysh/vtysh_ovsdb_config.h"
#include "vtysh/vtysh_ovsdb_if.h"
#include "vtysh_ovsdb_ntp_context.h"

VLOG_DEFINE_THIS_MODULE(vtysh_ntp_cli);

/* Global variables */
extern struct ovsdb_idl *idl;
//char g_NTP_prefer_default[]  = NTP_ASSOC_ATTRIB_PREFER_DEFAULT;
char g_NTP_version_default[] = NTP_ASSOC_ATTRIB_VERSION_DEFAULT;

/*================================================================================================*/
/* VRF Table Related functions */

/* Find the vrf with matching name */
static const struct ovsrec_vrf *
get_ovsrec_vrf_with_name(char *name)
{
    /* TODO change this later when multiple VRFs are supported */
    return ovsrec_vrf_first(idl);
}

/*================================================================================================*/
/* System Table Related functions */

static inline void
ntp_auth_enable_get_default_parameters(ntp_cli_ntp_auth_enable_params_t *pntp_auth_enable_params)
{
    memset(pntp_auth_enable_params, 0, sizeof(ntp_cli_ntp_auth_enable_params_t));
}

const int
vtysh_ovsdb_ntp_auth_enable_set(ntp_cli_ntp_auth_enable_params_t *pntp_auth_enable_params)
{
    const struct ovsrec_system *ovs_system = NULL;
    struct ovsdb_idl_txn *ntp_auth_enable_txn = NULL;

    /* Start of transaction */
    START_DB_TXN(ntp_auth_enable_txn);

    /* Get access to the System Table */
    ovs_system = ovsrec_system_first(idl);
    if (NULL == ovs_system) {
         vty_out(vty, "Could not access the System Table\n");
         ERRONEOUS_DB_TXN(ntp_auth_enable_txn, "Could not access the System Table");
    }

    if (pntp_auth_enable_params->no_form) {
        smap_replace((struct smap *)&ovs_system->ntp_config, SYSTEM_NTP_CONFIG_AUTHENTICATION_ENABLE, NTP_FALSE_STR);
    } else {
        smap_replace((struct smap *)&ovs_system->ntp_config, SYSTEM_NTP_CONFIG_AUTHENTICATION_ENABLE, NTP_TRUE_STR);
    }

    ovsrec_system_set_ntp_config(ovs_system, &ovs_system->ntp_config);

    /* End of transaction. */
    END_DB_TXN(ntp_auth_enable_txn);
}

/*================================================================================================*/
/* NTP internal server name validation functions */
static const bool
ntp_internal_has_all_digits(const char *pntp_server_name)
{
    while (*pntp_server_name) {
           if (!ispunct(*pntp_server_name) && !isdigit(*pntp_server_name)) {
               return false;
           }
           pntp_server_name++;
    }
    return true;
}

static const bool
ntp_internal_is_valid_ipv4_address(const char *pntp_server_ipv4_address)
{
    struct sockaddr_in sa;

    int result = inet_pton(AF_INET, pntp_server_ipv4_address, &(sa.sin_addr));

    if (result <= 0)
       return false;

    /* 0.0.0.0 - 0.255.255.255 are not valid host addresses */
    if (*pntp_server_ipv4_address == '0')
       return false;

    if(!IS_VALID_IPV4(htonl(sa.sin_addr.s_addr)))
        return false;

    return true;
}

static const bool
ntp_internal_is_valid_server_name(const char *pntp_server_name)
{
    if(!pntp_server_name) {
       return false;
    }

    if (ntp_internal_has_all_digits(pntp_server_name)) {
        return ntp_internal_is_valid_ipv4_address(pntp_server_name);
    }

    return true;
}

/*================================================================================================*/
/* NTP Keys Table Related functions */

static const struct ovsrec_ntp_key *
ntp_ovsrec_get_auth_key(int64_t key)
{
    int i = 0;
    const struct ovsrec_ntp_key *ntp_auth_key_row = NULL;

    OVSREC_NTP_KEY_FOR_EACH(ntp_auth_key_row, idl) {
        i++;

        if (ntp_auth_key_row->key_id == key) {
            VLOG_DBG("AuthKey matching %ld found at row = %d\n", key, i);
            return ntp_auth_key_row;
        }
    }

    VLOG_DBG("No matching auth-key found\n");
    return NULL;
}

static inline void
ntp_auth_key_get_default_parameters(ntp_cli_ntp_auth_key_params_t *pntp_auth_key_params)
{
    memset(pntp_auth_key_params, 0, sizeof(ntp_cli_ntp_auth_key_params_t));
}

static inline void
ntp_trusted_key_get_default_parameters(ntp_cli_ntp_trusted_key_params_t *pntp_trusted_key_params)
{
    memset(pntp_trusted_key_params, 0, sizeof(ntp_cli_ntp_trusted_key_params_t));
}

const int
ntp_auth_key_replace_parameters(const struct ovsrec_ntp_key *ntp_auth_key_row, ntp_cli_ntp_auth_key_params_t *pntp_auth_key_params, bool key_exists)
{
    if (ntp_auth_key_row) {
        if (!key_exists) {
            /* key_id is immutable. Set it only during row creation */
            ovsrec_ntp_key_set_key_id(ntp_auth_key_row, atoi(pntp_auth_key_params->key));
        }

        ovsrec_ntp_key_set_key_password(ntp_auth_key_row, pntp_auth_key_params->md5_pwd);
    }

    return CMD_SUCCESS;
}

/* Following function tests whether the value of keyid lies in the range [1-65534]
 * Also if requested it returns pointer to the row in the "NTP_Key" table
 */
const int
ntp_sanitize_auth_key(const char *pkey, const struct ovsrec_ntp_key **pntp_auth_key_row, char *password)
{
    /* Check key range */
    if (pkey) {
        int keyid = atoi(pkey);
        if ((keyid < NTP_KEY_KEY_ID_MIN) || (keyid > NTP_KEY_KEY_ID_MAX)) {
            vty_out(vty, "KeyID should lie between [%d-%d]\n", NTP_KEY_KEY_ID_MIN, NTP_KEY_KEY_ID_MAX);
            return CMD_ERR_NOTHING_TODO;
        }

        /* Check if the keyid exists */
        if (pntp_auth_key_row) {
            /* Row value is requested */

            *pntp_auth_key_row = ntp_ovsrec_get_auth_key(keyid);
            if (NULL == *pntp_auth_key_row) {
                vty_out(vty, "This key does not exist\n");
                return CMD_OVSDB_FAILURE;
            }
        }
    }

    if (password) {
        int pwdlen = strlen(password);

        if ((pwdlen < NTP_KEY_KEY_PASSWORD_LEN_MIN) || (pwdlen > NTP_KEY_KEY_PASSWORD_LEN_MAX)) {
            vty_out(vty, "Password length should be between %d & %d chars\n", NTP_KEY_KEY_PASSWORD_LEN_MIN, NTP_KEY_KEY_PASSWORD_LEN_MAX);
            return CMD_ERR_NOTHING_TODO;
        }
    }

    return CMD_SUCCESS;
}

const int
vtysh_ovsdb_ntp_auth_key_set(ntp_cli_ntp_auth_key_params_t *pntp_auth_key_params)
{
    const struct ovsrec_ntp_key *ntp_auth_key_row = NULL;
    struct ovsdb_idl_txn *ntp_auth_key_txn = NULL;
    bool key_exists = 0;
    int retval = CMD_SUCCESS;

    /* Sanitize the key & get the row in the NTP_Key Table */
    retval = ntp_sanitize_auth_key(pntp_auth_key_params->key, NULL, pntp_auth_key_params->md5_pwd);
    if (CMD_SUCCESS != retval) {
        return retval;
    }

    /* Start of transaction */
    START_DB_TXN(ntp_auth_key_txn);

    /* See if it already exists. */
    ntp_auth_key_row = ntp_ovsrec_get_auth_key(atoi(pntp_auth_key_params->key));
    if (NULL == ntp_auth_key_row) {
        if (pntp_auth_key_params->no_form) {
            /* Nothing to delete */
            vty_out(vty, "This key does not exist\n");
        } else {
            VLOG_DBG("Inserting a row into the NTP Keys table\n");

            ntp_auth_key_row = ovsrec_ntp_key_insert(ntp_auth_key_txn);
            if (NULL == ntp_auth_key_row) {
                VLOG_ERR("Could not insert a row into DB\n");
                ERRONEOUS_DB_TXN(ntp_auth_key_txn, "Could not insert a row into the NTP Keys Table");
            } else {
                VLOG_DBG("Inserted a row into the NTP Keys Table successfully\n");
                ntp_auth_key_replace_parameters(ntp_auth_key_row, pntp_auth_key_params, key_exists);
            }
        }
    } else {
        key_exists = 1;

        if (pntp_auth_key_params->no_form) {
            VLOG_DBG("Deleting a row from the NTP Keys table\n");
            ovsrec_ntp_key_delete(ntp_auth_key_row);
        } else {
            VLOG_DBG("This key already exists. Replacing password...\n");
            ntp_auth_key_replace_parameters(ntp_auth_key_row, pntp_auth_key_params, key_exists);
        }
    }

    /* End of transaction. */
    END_DB_TXN(ntp_auth_key_txn);
}

const int
vtysh_ovsdb_ntp_trusted_key_set(ntp_cli_ntp_trusted_key_params_t *pntp_trusted_key_params)
{
    const struct ovsrec_ntp_key *ntp_auth_key_row = NULL;
    struct ovsdb_idl_txn *ntp_trusted_key_txn = NULL;
    int64_t key = atoi(pntp_trusted_key_params->key);
    int retval = CMD_SUCCESS;

    /* Sanitize the key & get the row in the NTP_Key Table */
    retval = ntp_sanitize_auth_key(pntp_trusted_key_params->key, &ntp_auth_key_row, NULL);
    if (CMD_SUCCESS != retval) {
        return retval;
    }

    /* Start of transaction */
    START_DB_TXN(ntp_trusted_key_txn);

    /* See if it already exists. */
    if (NULL == ntp_auth_key_row) {
        vty_out(vty, "This key does not exist\n");
    } else {
        if (pntp_trusted_key_params->no_form) {
            VLOG_DBG("Unmarking key %ld as trusted\n", key);
            ovsrec_ntp_key_set_trust_enable(ntp_auth_key_row, NTP_FALSE);
        } else {
            VLOG_DBG("Marking key %ld as trusted\n", key);
            ovsrec_ntp_key_set_trust_enable(ntp_auth_key_row, NTP_TRUE);
        }
    }

    /* End of transaction. */
    END_DB_TXN(ntp_trusted_key_txn);
}

/*================================================================================================*/
/* NTP Association Table Related functions */

static const struct ovsrec_ntp_association *
ntp_ovsrec_get_assoc(char *vrf_name, char *server_name)
{
    int i = 0;
    const struct ovsrec_ntp_association *ntp_assoc_row = NULL;

    OVSREC_NTP_ASSOCIATION_FOR_EACH(ntp_assoc_row, idl) {
        i++;

        if (0 == strcmp(server_name, ntp_assoc_row->address)) {
            VLOG_DBG("Server matching %s found at row = %d. Now checking if the VRF it belongs to is \"%s\"...\n", server_name, i, vrf_name);

            if (NULL == ntp_assoc_row->vrf) {
                VLOG_ERR("No VRF associated with server %s\n", server_name);
            } else {
                VLOG_DBG("Corresponding vrf name = %s\n", ntp_assoc_row->vrf->name);

                /* Now check if it is for the intended VRF */
                if (0 == strcmp(ntp_assoc_row->vrf->name, vrf_name)) {
                    VLOG_DBG("Server record found at row = %d\n", i);
                    return ntp_assoc_row;
                }
            }
        }
    }

    VLOG_DBG("No matching server record found\n");
    return NULL;
}

static inline void
ntp_server_get_default_cfg(ntp_cli_ntp_server_params_t *pntp_server_params)
{
    memset(pntp_server_params, 0, sizeof(ntp_cli_ntp_server_params_t));
    //pntp_server_params->prefer = g_NTP_prefer_default;
    pntp_server_params->version = g_NTP_version_default;
}

const int
ntp_server_replace_parameters(const struct ovsrec_ntp_association *ntp_assoc_row, ntp_cli_ntp_server_params_t *ntp_server_params, bool server_exists)
{
    struct smap smap_assoc_attribs;
    struct smap smap_assoc_status;
    const struct smap *psmap = NULL;

    if (ntp_assoc_row) {
        psmap = &ntp_assoc_row->association_attributes;
        smap_clone(&smap_assoc_attribs, psmap);

        if (!server_exists) {
            /* The following are immutable fields. Set them only the 1st time the row is getting inserted */

            /* Set the server name */
            ovsrec_ntp_association_set_address(ntp_assoc_row, ntp_server_params->server_name);

            /* Set the VRF name */
            /* TODO: set the "n_vrf" parameter for the following function call correctly when multiple VRFs are supported */
            const struct ovsrec_vrf *vrf_row = get_ovsrec_vrf_with_name(ntp_server_params->vrf_name);
            ovsrec_ntp_association_set_vrf(ntp_assoc_row, vrf_row);

            /* Set default values for other columns */
            ovsrec_ntp_association_set_key_id(ntp_assoc_row, NULL);

            /* Set default values for assoc_attributes */
            smap_replace(&smap_assoc_attribs, NTP_ASSOC_ATTRIB_REF_CLOCK_ID, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_attribs, NTP_ASSOC_ATTRIB_PREFER, NTP_FALSE_STR);
            smap_replace(&smap_assoc_attribs, NTP_ASSOC_ATTRIB_VERSION, NTP_ASSOC_ATTRIB_VERSION_DEFAULT);

            /* Set default values for status parameters (operational data) */
            psmap = &ntp_assoc_row->association_status;
            smap_clone(&smap_assoc_status, psmap);

            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_REMOTE_PEER_ADDRESS, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_REMOTE_PEER_REF_ID, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_STRATUM, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_PEER_TYPE, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_LAST_POLLED, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_POLLING_INTERVAL, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_REACHABILITY_REGISTER, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_NETWORK_DELAY, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_TIME_OFFSET, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_JITTER, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_ROOT_DISPERSION, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_REFERENCE_TIME, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_PEER_STATUS_WORD, NTP_DEFAULT_STR);
            smap_replace(&smap_assoc_status, NTP_ASSOC_STATUS_ASSOCID, NTP_DEFAULT_STR);

            ovsrec_ntp_association_set_association_status(ntp_assoc_row, &smap_assoc_status);
            smap_destroy(&smap_assoc_status);
        }

        if (ntp_server_params->prefer) {
            smap_replace(&smap_assoc_attribs, NTP_ASSOC_ATTRIB_PREFER, NTP_TRUE_STR);
        }

        if (ntp_server_params->version) {
            smap_replace(&smap_assoc_attribs, NTP_ASSOC_ATTRIB_VERSION, ntp_server_params->version);
        }

        if (ntp_server_params->keyid) {
            ovsrec_ntp_association_set_key_id(ntp_assoc_row, (struct ovsrec_ntp_key *)ntp_server_params->key_row);
        }

        ovsrec_ntp_association_set_association_attributes(ntp_assoc_row, &smap_assoc_attribs);
        smap_destroy(&smap_assoc_attribs);
    }

    return CMD_SUCCESS;
}

const int
ntp_server_sanitize_parameters(ntp_cli_ntp_server_params_t *pntp_server_params)
{
    int retval = CMD_SUCCESS;

    /* Check server_name to see if it exceeds maximum number of characters*/
    if (strlen(pntp_server_params->server_name) > MAX_CHARS_IN_NTP_SERVER_NAME) {
        vty_out(vty, "NTP server name should be less than 57 characters\n");
        return CMD_ERR_NOTHING_TODO;
    }

    /* Check the validity of server name */
    if (!ntp_internal_is_valid_server_name(pntp_server_params->server_name)) {
        vty_out(vty, "Invalid IP address%s", VTY_NEWLINE);
        return CMD_ERR_NOTHING_TODO;
    }

    /* Check sanity for the key */
    if (pntp_server_params->keyid) {
        retval = ntp_sanitize_auth_key(pntp_server_params->keyid, (const struct ovsrec_ntp_key **)(&(pntp_server_params->key_row)), NULL);
        if (CMD_SUCCESS != retval) {
            return retval;
        }
    }

    /* Check sanity for the version */
    if (pntp_server_params->version) {
        int ntp_ver = atoi(pntp_server_params->version);

        if ((ntp_ver < atoi(NTP_ASSOC_ATTRIB_VERSION_3)) || (ntp_ver > atoi(NTP_ASSOC_ATTRIB_VERSION_4))) {
            vty_out(vty, "NTP version should lie between [%s-%s]\n", NTP_ASSOC_ATTRIB_VERSION_3, NTP_ASSOC_ATTRIB_VERSION_4);
            return CMD_ERR_NOTHING_TODO;
        }
    }

    return CMD_SUCCESS;
}

const int
ntp_server_check_max_number_of_servers(ntp_cli_ntp_server_params_t *pntp_server_params)
{
    /* Check for more than 8 NTP servers */
    if (!pntp_server_params->no_form) {
        int counter = 0;
        const struct ovsrec_ntp_association *ntp_assoc_row = NULL;
        OVSREC_NTP_ASSOCIATION_FOR_EACH(ntp_assoc_row, idl) {
            counter++;
        }

        if (counter >= NTP_ASSOC_MAX_SERVERS) {
            vty_out (vty, "Maximum number of configurable"
                          " NTP server limit has been reached%s",
                     VTY_NEWLINE);
            return CMD_ERR_NOTHING_TODO;
        }
    }

    return CMD_SUCCESS;
}

const int
vtysh_ovsdb_ntp_server_set(ntp_cli_ntp_server_params_t *ntp_server_params)
{
    const struct ovsrec_ntp_association *ntp_assoc_row = NULL;
    struct ovsdb_idl_txn *ntp_association_txn = NULL;
    bool ntp_server_exists = 0;
    int retval = CMD_SUCCESS;

    retval = ntp_server_sanitize_parameters(ntp_server_params);
    if (CMD_SUCCESS != retval) {
        return retval;
    }

    /* Start of transaction */
    START_DB_TXN(ntp_association_txn);

    /* See if it already exists. */
    ntp_assoc_row = ntp_ovsrec_get_assoc(ntp_server_params->vrf_name, ntp_server_params->server_name);
    if (NULL == ntp_assoc_row) {
        if (ntp_server_params->no_form) {
            /* Nothing to delete */
            vty_out(vty, "This server does not exist\n");
        } else {

            retval = ntp_server_check_max_number_of_servers(ntp_server_params);
            if (CMD_SUCCESS != retval) {
                /* End of transaction. */
                END_DB_TXN(ntp_association_txn);
                return retval;
            }
            VLOG_DBG("Inserting a row into the NTP Assoc table\n");

            ntp_assoc_row = ovsrec_ntp_association_insert(ntp_association_txn);
            if (NULL == ntp_assoc_row) {
                VLOG_ERR("Could not insert a row into the NTP Assoc Table\n");
                ERRONEOUS_DB_TXN(ntp_association_txn, "Could not insert a row into the NTP Assoc Table");
            } else {
                VLOG_DBG("Inserted a row into the NTP Assoc Table successfully\n");
                ntp_server_replace_parameters(ntp_assoc_row, ntp_server_params, ntp_server_exists);
            }
        }
    } else {
        ntp_server_exists = 1;

        if (ntp_server_params->no_form) {
            VLOG_DBG("Deleting a row from the NTP Assoc table\n");
            ovsrec_ntp_association_delete(ntp_assoc_row);
        } else {
            VLOG_DBG("This server already exists. Replacing parameters\n");
            ntp_server_replace_parameters(ntp_assoc_row, ntp_server_params, ntp_server_exists);
        }
    }

    /* End of transaction. */
    END_DB_TXN(ntp_association_txn);
}

/*================================================================================================*/
/* SHOW CLI Implementations */

static void
vtysh_ovsdb_show_ntp_associations()
{
    const struct ovsrec_ntp_association *ntp_assoc_row = NULL;
    int i = 0;
    const char *buf = NULL;
    char ser_name[39];
    char rem_info[16];

    vty_out(vty, "------------------------------------------------------------------------------"
                 "----------------------------------------------------------------\n");
    vty_out(vty, " %3s  %39s  %15s  %3s  %5s",
        "ID", "NAME", "REMOTE", "VER", "KEYID");
    vty_out(vty, "  %15s  %2s  %1s  %4s  %4s  %5s  %7s  %6s  %6s\n",
        "REF-ID", "ST", "T", "LAST", "POLL", "REACH", "DELAY", "OFFSET", "JITTER");
    vty_out(vty, "------------------------------------------------------------------------------"
                 "----------------------------------------------------------------\n");

    OVSREC_NTP_ASSOCIATION_FOR_EACH(ntp_assoc_row, idl) {
        buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_PEER_STATUS_WORD);
        if (buf) {
            if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_STATUS_WORD_REJECT)) {
                vty_out(vty, " ");
            } else if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_STATUS_WORD_FALSETICK)) {
                vty_out(vty, "x");
            } else if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_STATUS_WORD_EXCESS)) {
                vty_out(vty, ".");
            } else if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_STATUS_WORD_OUTLIER)) {
                vty_out(vty, "-");
            } else if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_STATUS_WORD_CANDIDATE)) {
                vty_out(vty, "+");
            } else if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_STATUS_WORD_BACKUP)) {
                vty_out(vty, "#");
            } else if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_STATUS_WORD_SYSTEMPEER)) {
                vty_out(vty, "*");
            } else if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_STATUS_WORD_PPSPEER)) {
                vty_out(vty, "o");
            } else {
                vty_out(vty, " "); /* Value not set - Same as NTP_ASSOC_STATUS_PEER_STATUS_WORD_REJECT */
            }
        } else {
            vty_out(vty, " "); /* Value not set - Same as NTP_ASSOC_STATUS_PEER_STATUS_WORD_REJECT */
        }

        vty_out(vty, "%3d", ++i);

        snprintf(ser_name, sizeof(ser_name), "%s", ntp_assoc_row->address);
        vty_out(vty, "  %39s", ser_name);


        buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_REMOTE_PEER_ADDRESS);
        snprintf(rem_info, sizeof(rem_info), "%s", buf);
        vty_out(vty, "  %15s", rem_info);

        buf = smap_get(&ntp_assoc_row->association_attributes, NTP_ASSOC_ATTRIB_VERSION);
        vty_out(vty, "  %3s", ((buf) ? buf : ""));

        if (ntp_assoc_row->key_id) {
            vty_out(vty, "  %5ld", ((struct ovsrec_ntp_key *)ntp_assoc_row->key_id)->key_id);
        } else {
            vty_out(vty, "  %5s", NTP_DEFAULT_STR);
        }

        buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_REMOTE_PEER_REF_ID);
        snprintf(rem_info, sizeof(rem_info), "%s", buf);
        vty_out(vty, "  %15s", rem_info);

        buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_STRATUM);
        vty_out(vty, "  %2s", ((buf) ? buf : ""));

        buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_PEER_TYPE);
        if (buf) {
            if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_TYPE_UNI_MANY_CAST)) {
                vty_out(vty, "  U");
            } else if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_TYPE_B_M_CAST)) {
                vty_out(vty, "  b");
            } else if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_TYPE_LOCAL_REF_CLOCK)) {
                vty_out(vty, "  L");
            } else if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_TYPE_SYMM_PEER)) {
                vty_out(vty, "  S");
            } else if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_TYPE_MANYCAST)) {
                vty_out(vty, "  m");
            } else if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_TYPE_BROADCAST)) {
                vty_out(vty, "  B");
            } else if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_TYPE_MULTICAST)) {
                vty_out(vty, "  M");
            } else {
                vty_out(vty, "  -");
            }
        } else {
            vty_out(vty, "  -");
        }

        buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_LAST_POLLED);
        vty_out(vty, "  %4s", ((buf) ? buf : ""));

        buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_POLLING_INTERVAL);
        vty_out(vty, "  %4s", ((buf) ? buf : ""));

        buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_REACHABILITY_REGISTER);
        vty_out(vty, "  %5s", ((buf) ? buf : ""));

        buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_NETWORK_DELAY);
        vty_out(vty, "  %7s", ((buf) ? buf : ""));

        buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_TIME_OFFSET);
        vty_out(vty, "  %6s", ((buf) ? buf : ""));

        buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_JITTER);
        vty_out(vty, "  %6s", ((buf) ? buf : ""));

        vty_out(vty, "\n");
    }

    vty_out(vty, "------------------------------------------------------------------------------"
                 "----------------------------------------------------------------\n");
}

static void
vtysh_ovsdb_show_ntp_status()
{
    const struct ovsrec_system *ovs_system = NULL;
    const char *buf = NULL;
    const struct ovsrec_ntp_association *ntp_assoc_row = NULL;
    bool status = 0;

    /* Get access to the System Table */
    ovs_system = ovsrec_system_first(idl);
    if (NULL == ovs_system) {
         vty_out(vty, "Could not access the System Table\n");
         return;
    }

    vty_out(vty, "NTP is enabled\n");

    status = smap_get_bool(&ovs_system->ntp_config, SYSTEM_NTP_CONFIG_AUTHENTICATION_ENABLE, false);
    vty_out(vty, "NTP authentication is %s\n", ((status) ? SYSTEM_NTP_CONFIG_AUTHENTICATION_ENABLED : SYSTEM_NTP_CONFIG_AUTHENTICATION_DISABLED));

    buf = smap_get(&ovs_system->ntp_status, SYSTEM_NTP_STATUS_UPTIME);
    vty_out(vty, "Uptime: %s second(s)\n", ((buf) ? buf : NTP_DEFAULT_STR));

    OVSREC_NTP_ASSOCIATION_FOR_EACH(ntp_assoc_row, idl) {
        buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_PEER_STATUS_WORD);
        if (buf) {
            VLOG_DBG("System status word: %s\n", buf);

            if (0 == strcmp(buf, NTP_ASSOC_STATUS_PEER_STATUS_WORD_SYSTEMPEER)) {
                buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_STRATUM);
                vty_out(vty, "Synchronized to NTP Server %s at stratum %s\n", ntp_assoc_row->address, ((buf) ? buf : ""));

                buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_POLLING_INTERVAL);
                vty_out(vty, "Poll interval = %s seconds\n", ((buf) ? buf : ""));

                buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_TIME_OFFSET);
                vty_out(vty, "Time accuracy is within %s seconds\n", ((buf) ? buf : ""));

                buf = smap_get(&ntp_assoc_row->association_status, NTP_ASSOC_STATUS_REFERENCE_TIME);
                vty_out(vty, "Reference time: %s (UTC)\n", ((buf) ? buf : ""));
            }
        }
    }
}

static void
vtysh_ovsdb_show_ntp_statistics()
{
    const struct ovsrec_system *ovs_system = NULL;
    const char *buf = NULL;

    /* Get access to the System Table */
    ovs_system = ovsrec_system_first(idl);
    if (NULL == ovs_system) {
         vty_out(vty, "Could not access the System Table\n");
         return;
    }

    buf = smap_get(&ovs_system->ntp_statistics, SYSTEM_NTP_STATS_PKTS_RCVD);
    vty_out(vty, "%20s    %s\n", "Rx-pkts", ((buf) ? buf : NTP_DEFAULT_STR));

    buf = smap_get(&ovs_system->ntp_statistics, SYSTEM_NTP_STATS_PKTS_CUR_VER);
    vty_out(vty, "%20s    %s\n", "Cur Ver Rx-pkts", ((buf) ? buf : NTP_DEFAULT_STR));

    buf = smap_get(&ovs_system->ntp_statistics, SYSTEM_NTP_STATS_PKTS_OLD_VER);
    vty_out(vty, "%20s    %s\n", "Old Ver Rx-pkts", ((buf) ? buf : NTP_DEFAULT_STR));

    buf = smap_get(&ovs_system->ntp_statistics, SYSTEM_NTP_STATS_PKTS_BAD_LEN_OR_FORMAT);
    vty_out(vty, "%20s    %s\n", "Error pkts", ((buf) ? buf : NTP_DEFAULT_STR));

    buf = smap_get(&ovs_system->ntp_statistics, SYSTEM_NTP_STATS_PKTS_AUTH_FAILED);
    vty_out(vty, "%20s    %s\n", "Auth-failed pkts", ((buf) ? buf : NTP_DEFAULT_STR));

    buf = smap_get(&ovs_system->ntp_statistics, SYSTEM_NTP_STATS_PKTS_DECLINED);
    vty_out(vty, "%20s    %s\n", "Declined pkts", ((buf) ? buf : NTP_DEFAULT_STR));

    buf = smap_get(&ovs_system->ntp_statistics, SYSTEM_NTP_STATS_PKTS_RESTRICTED);
    vty_out(vty, "%20s    %s\n", "Restricted pkts", ((buf) ? buf : NTP_DEFAULT_STR));

    buf = smap_get(&ovs_system->ntp_statistics, SYSTEM_NTP_STATS_PKTS_RATE_LIMITED);
    vty_out(vty, "%20s    %s\n", "Rate-limited pkts", ((buf) ? buf : NTP_DEFAULT_STR));

    buf = smap_get(&ovs_system->ntp_statistics, SYSTEM_NTP_STATS_PKTS_KOD_RESPONSES);
    vty_out(vty, "%20s    %s\n", "KOD pkts", ((buf) ? buf : NTP_DEFAULT_STR));
}

static void
vtysh_ovsdb_show_ntp_trusted_keys()
{
    const struct ovsrec_ntp_key *ntp_auth_key_row = NULL;

    vty_out(vty,"------------\n");
    vty_out(vty,"Trusted-keys\n");
    vty_out(vty,"------------\n");

    OVSREC_NTP_KEY_FOR_EACH(ntp_auth_key_row, idl) {
        if ((ntp_auth_key_row) && (ntp_auth_key_row->trust_enable)) {
            vty_out(vty, "%ld\n", ntp_auth_key_row->key_id);
        }
    }

    vty_out(vty,"------------\n");
}

static void
vtysh_ovsdb_show_ntp_authentication_keys()
{
    const struct ovsrec_ntp_key *ntp_auth_key_row = NULL;

    vty_out(vty,"---------------------------\n");
    vty_out(vty,"%8s   %16s\n", "Auth-key", "MD5 password");
    vty_out(vty,"---------------------------\n");

    OVSREC_NTP_KEY_FOR_EACH(ntp_auth_key_row, idl) {
        if (ntp_auth_key_row) {
            vty_out(vty, "%8ld   %16s\n", ntp_auth_key_row->key_id, ntp_auth_key_row->key_password);
        }
    }

    vty_out(vty,"---------------------------\n");
}

/*================================================================================================*/
/* CLI Definitions */

/* SHOW CLIs */
DEFUN ( vtysh_show_ntp_associations,
        vtysh_show_ntp_associations_cmd,
        "show ntp associations",
        SHOW_STR
        NTP_SHOW_STR
        NTP_SHOW_ASSOC_STR
      )
{
    vtysh_ovsdb_show_ntp_associations();
    return CMD_SUCCESS;
}

DEFUN ( vtysh_show_ntp_status,
        vtysh_show_ntp_status_cmd,
        "show ntp status",
        SHOW_STR
        NTP_SHOW_STR
        NTP_SHOW_STATUS_STR
      )
{
    vtysh_ovsdb_show_ntp_status();
    return CMD_SUCCESS;
}

DEFUN ( vtysh_show_ntp_statistics,
        vtysh_show_ntp_statistics_cmd,
        "show ntp statistics",
        SHOW_STR
        NTP_SHOW_STR
        NTP_SHOW_STATISTICS_STR
      )
{
    vtysh_ovsdb_show_ntp_statistics();
    return CMD_SUCCESS;
}

DEFUN ( vtysh_show_ntp_trusted_keys,
        vtysh_show_ntp_trusted_keys_cmd,
        "show ntp trusted-keys",
        SHOW_STR
        NTP_SHOW_STR
        NTP_SHOW_TRUST_KEYS_STR
      )
{
    vtysh_ovsdb_show_ntp_trusted_keys();
    return CMD_SUCCESS;
}

DEFUN ( vtysh_show_ntp_authentication_keys,
        vtysh_show_ntp_authentication_keys_cmd,
        "show ntp authentication-keys",
        SHOW_STR
        NTP_SHOW_STR
        NTP_SHOW_AUTH_KEYS_STR
      )
{
    vtysh_ovsdb_show_ntp_authentication_keys();
    return CMD_SUCCESS;
}

/* CONFIG CLIs */
DEFUN ( vtysh_set_ntp_server,
        vtysh_set_ntp_server_cmd,
        "ntp server WORD "
        "{prefer | version <3-4> | key-id <1-65534>}",
        NTP_STR
        NTP_SERVER_STR
        NTP_SERVER_NAME_STR
        NTP_SERVER_PREFER_STR
        NTP_SERVER_VERSION_STR
        NTP_SERVER_VERSION_NUM_STR
        NTP_KEY_ID_STR
        NTP_KEY_NUM_STR
      )
{
    int ret_code = CMD_SUCCESS;
    ntp_cli_ntp_server_params_t ntp_server_params;
    ntp_server_get_default_cfg(&ntp_server_params);

    /* Set various parameters needed by the "ntp server" command handler */
    ntp_server_params.vrf_name = DEFAULT_VRF_NAME;
    ntp_server_params.server_name = (char *)argv[0];
    ntp_server_params.prefer = (char *)argv[1];
    ntp_server_params.version = (char *)argv[2];
    ntp_server_params.keyid = (char *)argv[3];

    if (vty_flags & CMD_FLAG_NO_CMD) {
        ntp_server_params.no_form = 1;

        ntp_server_params.prefer = NULL;
        ntp_server_params.version = NULL;
        ntp_server_params.keyid = NULL;
    }

    /* Finally call the handler */
    ret_code = vtysh_ovsdb_ntp_server_set(&ntp_server_params);

    return ret_code;
}


DEFUN_NO_FORM ( vtysh_set_ntp_server,
        vtysh_set_ntp_server_cmd,
        "ntp server WORD",
        NTP_STR
        NTP_SERVER_STR
        NTP_SERVER_NAME_STR
      );


DEFUN ( vtysh_set_ntp_authentication_enable,
        vtysh_set_ntp_authentication_enable_cmd,
        "ntp authentication enable",
        NTP_STR
        NTP_AUTH_STR
        NTP_AUTH_ENABLE_STR
      )
{
    int ret_code = CMD_SUCCESS;
    ntp_cli_ntp_auth_enable_params_t ntp_auth_enable_params;
    ntp_auth_enable_get_default_parameters(&ntp_auth_enable_params);

    /* Set various parameters needed by the "ntp authentication enable" command handler */
    if (vty_flags & CMD_FLAG_NO_CMD) {
        ntp_auth_enable_params.no_form = 1;
    }

    /* Finally call the handler */
    ret_code = vtysh_ovsdb_ntp_auth_enable_set(&ntp_auth_enable_params);

    return ret_code;
}


DEFUN_NO_FORM ( vtysh_set_ntp_authentication_enable,
        vtysh_set_ntp_authentication_enable_cmd,
        "ntp authentication enable",
        NTP_STR
        NTP_AUTH_STR
        NTP_AUTH_ENABLE_STR
      );


DEFUN ( vtysh_set_ntp_authentication_key,
        vtysh_set_ntp_authentication_key_cmd,
        "ntp authentication-key <1-65534> md5 WORD",
        NTP_STR
        NTP_AUTH_KEY_STR
        NTP_KEY_NUM_STR
        NTP_MD5_STR
        NTP_MD5_PASSWORD_STR
      )
{
    int ret_code = CMD_SUCCESS;
    ntp_cli_ntp_auth_key_params_t ntp_auth_key_params;
    ntp_auth_key_get_default_parameters(&ntp_auth_key_params);

    /* Set various parameters needed by the "ntp auth key" command handler */
    ntp_auth_key_params.key = (char *)argv[0];
    ntp_auth_key_params.md5_pwd = (char *)argv[1];

    if (vty_flags & CMD_FLAG_NO_CMD) {
        ntp_auth_key_params.no_form = 1;
        ntp_auth_key_params.md5_pwd = NULL;
    }

    /* Finally call the handler */
    ret_code = vtysh_ovsdb_ntp_auth_key_set(&ntp_auth_key_params);

    return ret_code;
}


DEFUN_NO_FORM ( vtysh_set_ntp_authentication_key,
        vtysh_set_ntp_authentication_key_cmd,
        "ntp authentication-key <1-65534>",
        NTP_STR
        NTP_AUTH_KEY_STR
        NTP_KEY_NUM_STR
      );


DEFUN ( vtysh_set_ntp_trusted_key,
        vtysh_set_ntp_trusted_key_cmd,
        "ntp trusted-key <1-65534>",
        NTP_STR
        NTP_TRUST_KEY_STR
        NTP_KEY_NUM_STR
      )
{
    int ret_code = CMD_SUCCESS;
    ntp_cli_ntp_trusted_key_params_t ntp_trusted_key_params;
    ntp_trusted_key_get_default_parameters(&ntp_trusted_key_params);

    /* Set various parameters needed by the "ntp trusted key" command handler */
    ntp_trusted_key_params.key = (char *)argv[0];

    if (vty_flags & CMD_FLAG_NO_CMD) {
        ntp_trusted_key_params.no_form = 1;
    }

    /* Finally call the handler */
    ret_code = vtysh_ovsdb_ntp_trusted_key_set(&ntp_trusted_key_params);

    return ret_code;
}


DEFUN_NO_FORM ( vtysh_set_ntp_trusted_key,
        vtysh_set_ntp_trusted_key_cmd,
        "ntp trusted-key <1-65534>",
        NTP_STR
        NTP_TRUST_KEY_STR
        NTP_KEY_NUM_STR
      );

/*================================================================================================*/

static void
ntp_ovsdb_init()
{
    /* Add System Table */
    ovsdb_idl_add_table(idl, &ovsrec_table_system);

    /* Add columns in System Table */
    ovsdb_idl_add_column(idl, &ovsrec_system_col_ntp_config);
    ovsdb_idl_add_column(idl, &ovsrec_system_col_ntp_statistics);
    ovsdb_idl_add_column(idl, &ovsrec_system_col_ntp_status);

    /* Add VRF Table */
    ovsdb_idl_add_table(idl, &ovsrec_table_vrf);

    /* Add columns in VRF Table */
    ovsdb_idl_add_column(idl, &ovsrec_vrf_col_name);

    /* Add NTP Association Table */
    ovsdb_idl_add_table(idl, &ovsrec_table_ntp_association);

    /* Add columns in NTP Association Table */
    ovsdb_idl_add_column(idl, &ovsrec_ntp_association_col_address);
    ovsdb_idl_add_column(idl, &ovsrec_ntp_association_col_association_status);
    ovsdb_idl_add_column(idl, &ovsrec_ntp_association_col_vrf);
    ovsdb_idl_add_column(idl, &ovsrec_ntp_association_col_key_id);
    ovsdb_idl_add_column(idl, &ovsrec_ntp_association_col_association_attributes);

    /* Add NTP Keys Table */
    ovsdb_idl_add_table(idl, &ovsrec_table_ntp_key);

    /* Add columns in NTP Keys Table */
    ovsdb_idl_add_column(idl, &ovsrec_ntp_key_col_key_id);
    ovsdb_idl_add_column(idl, &ovsrec_ntp_key_col_key_password);
    ovsdb_idl_add_column(idl, &ovsrec_ntp_key_col_trust_enable);
}

/* Initialize ops-ntpd cli node.
 */
void cli_pre_init(void)
{
    /* ops-ntpd doesn't have any context level cli commands.
     * To load ops-ntpd cli shared libraries at runtime, this function is required.
     */
    /* Add tables/columns needed for NTP config commands */
    ntp_ovsdb_init();
}

/* Initialize ops-ntpd cli element.
 */
void cli_post_init(void)
{
    vtysh_ret_val retval = e_vtysh_error;

    /* SHOW CMDS */
    install_element (VIEW_NODE, &vtysh_show_ntp_associations_cmd);
    install_element (ENABLE_NODE, &vtysh_show_ntp_associations_cmd);

    install_element (VIEW_NODE, &vtysh_show_ntp_status_cmd);
    install_element (ENABLE_NODE, &vtysh_show_ntp_status_cmd);

    install_element (VIEW_NODE, &vtysh_show_ntp_statistics_cmd);
    install_element (ENABLE_NODE, &vtysh_show_ntp_statistics_cmd);

    install_element (VIEW_NODE, &vtysh_show_ntp_trusted_keys_cmd);
    install_element (ENABLE_NODE, &vtysh_show_ntp_trusted_keys_cmd);

    install_element (VIEW_NODE, &vtysh_show_ntp_authentication_keys_cmd);
    install_element (ENABLE_NODE, &vtysh_show_ntp_authentication_keys_cmd);

    /* CONFIG CMDS */
    install_element (CONFIG_NODE, &vtysh_set_ntp_server_cmd);
    install_element (CONFIG_NODE, &no_vtysh_set_ntp_server_cmd);

    install_element (CONFIG_NODE, &vtysh_set_ntp_authentication_enable_cmd);
    install_element (CONFIG_NODE, &no_vtysh_set_ntp_authentication_enable_cmd);

    install_element (CONFIG_NODE, &vtysh_set_ntp_authentication_key_cmd);
    install_element (CONFIG_NODE, &no_vtysh_set_ntp_authentication_key_cmd);

    install_element (CONFIG_NODE, &vtysh_set_ntp_trusted_key_cmd);
    install_element (CONFIG_NODE, &no_vtysh_set_ntp_trusted_key_cmd);

    /* Installing running config sub-context with global config context */
    retval = install_show_run_config_subcontext(e_vtysh_config_context,
                                     e_vtysh_config_context_ntp,
                                     &vtysh_config_context_ntp_clientcallback,
                                     NULL, NULL);
    if(e_vtysh_ok != retval)
    {
        vtysh_ovsdb_config_logmsg(VTYSH_OVSDB_CONFIG_ERR,
                           "config context unable to add ntp client callback");
        assert(0);
    }
}

/*================================================================================================*/
