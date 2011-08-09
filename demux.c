/*****************************************************************************
 * demux.c
 *****************************************************************************
 * Copyright (C) 2004, 2008-2011 VideoLAN
 * $Id$
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *          Andy Gatward <a.j.gatward@reading.ac.uk>
 *          Marian Ďurkovič <md@bts.sk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "dvblast.h"
#include "en50221.h"

#ifdef HAVE_ICONV
#include <iconv.h>
#endif

#include <bitstream/mpeg/ts.h>
#include <bitstream/mpeg/pes.h>
#include <bitstream/mpeg/psi.h>
#include <bitstream/dvb/si.h>
#include <bitstream/dvb/si_print.h>
#include <bitstream/mpeg/psi_print.h>

extern bool b_enable_emm;
extern bool b_enable_ecm;

/*****************************************************************************
 * Local declarations
 *****************************************************************************/
#define MAX_PIDS                8192
#define MIN_SECTION_FRAGMENT    PSI_HEADER_SIZE_SYNTAX1

typedef struct ts_pid_t
{
    int i_refcount;
    int i_psi_refcount;
    bool b_pes;
    int8_t i_last_cc;
    int i_demux_fd;
    /* b_emm is set to true when PID carries EMM packet
       and should be outputed in all services */
    bool b_emm;

    /* biTStream PSI section gathering */
    uint8_t *p_psi_buffer;
    uint16_t i_psi_buffer_used;

    output_t **pp_outputs;
    int i_nb_outputs;
} ts_pid_t;

typedef struct sid_t
{
    uint16_t i_sid, i_pmt_pid;
    uint8_t *p_current_pmt;
} sid_t;

ts_pid_t p_pids[MAX_PIDS];
static sid_t **pp_sids = NULL;
static int i_nb_sids = 0;

static PSI_TABLE_DECLARE(pp_current_pat_sections);
static PSI_TABLE_DECLARE(pp_next_pat_sections);
static PSI_TABLE_DECLARE(pp_current_cat_sections);
static PSI_TABLE_DECLARE(pp_next_cat_sections);
static PSI_TABLE_DECLARE(pp_current_nit_sections);
static PSI_TABLE_DECLARE(pp_next_nit_sections);
static PSI_TABLE_DECLARE(pp_current_sdt_sections);
static PSI_TABLE_DECLARE(pp_next_sdt_sections);
static mtime_t i_last_dts = -1;
static int i_demux_fd;
static int i_nb_errors = 0;
static mtime_t i_last_error = 0;

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void demux_Handle( block_t *p_ts );
static void SetDTS( block_t *p_list );
static void SetPID( uint16_t i_pid );
static void SetPID_EMM( uint16_t i_pid );
static void UnsetPID( uint16_t i_pid );
static void StartPID( output_t *p_output, uint16_t i_pid );
static void StopPID( output_t *p_output, uint16_t i_pid );
static void SelectPID( uint16_t i_sid, uint16_t i_pid );
static void UnselectPID( uint16_t i_sid, uint16_t i_pid );
static void SelectPMT( uint16_t i_sid, uint16_t i_pid );
static void UnselectPMT( uint16_t i_sid, uint16_t i_pid );
static void GetPIDS( uint16_t **ppi_wanted_pids, int *pi_nb_wanted_pids,
                     uint16_t i_sid,
                     const uint16_t *pi_pids, int i_nb_pids );
static bool SIDIsSelected( uint16_t i_sid );
static bool PIDWouldBeSelected( uint8_t *p_es );
static bool PMTNeedsDescrambling( uint8_t *p_pmt );
static void FlushEIT( output_t *p_output, mtime_t i_dts );
static void SendTDT( block_t *p_ts );
static void NewPAT( output_t *p_output );
static void NewPMT( output_t *p_output );
static void NewNIT( output_t *p_output );
static void NewSDT( output_t *p_output );
static void HandlePSIPacket( uint8_t *p_ts, mtime_t i_dts );

/*****************************************************************************
 * demux_Open
 *****************************************************************************/
void demux_Open( void )
{
    int i;

    memset( p_pids, 0, sizeof(p_pids) );

    pf_Open();

    for ( i = 0; i < 8192; i++ )
    {
        p_pids[i].i_last_cc = -1;
        p_pids[i].i_demux_fd = -1;
        psi_assemble_init( &p_pids[i].p_psi_buffer,
                           &p_pids[i].i_psi_buffer_used );
    }

    if ( b_budget_mode )
        i_demux_fd = pf_SetFilter(8192);

    psi_table_init( pp_current_pat_sections );
    psi_table_init( pp_next_pat_sections );
    SetPID(PAT_PID);
    p_pids[PAT_PID].i_psi_refcount++;

    if ( b_enable_emm ) {
        psi_table_init( pp_current_cat_sections );
        psi_table_init( pp_next_cat_sections );
        SetPID_EMM(CAT_PID);
        p_pids[CAT_PID].i_psi_refcount++;
    }

    SetPID(NIT_PID);
    p_pids[NIT_PID].i_psi_refcount++;

    psi_table_init( pp_current_sdt_sections );
    psi_table_init( pp_next_sdt_sections );
    SetPID(SDT_PID);
    p_pids[SDT_PID].i_psi_refcount++;

    SetPID(EIT_PID);
    p_pids[EIT_PID].i_psi_refcount++;

    SetPID(RST_PID);

    SetPID(TDT_PID);
}

/*****************************************************************************
 * demux_Run
 *****************************************************************************/
void demux_Run( block_t *p_ts )
{
    SetDTS( p_ts );

    while ( p_ts != NULL )
    {
        block_t *p_next = p_ts->p_next;
        p_ts->p_next = NULL;
        demux_Handle( p_ts );
        p_ts = p_next;
    }
}

/*****************************************************************************
 * demux_Handle
 *****************************************************************************/
static void demux_Handle( block_t *p_ts )
{
    uint16_t i_pid = ts_get_pid( p_ts->p_ts );
    uint8_t i_cc = ts_get_cc( p_ts->p_ts );
    int i;

    if ( !ts_validate( p_ts->p_ts ) )
    {
        msg_Warn( NULL, "lost TS sync" );
        switch ( i_print_type )
        {
        case PRINT_XML:
            printf("<ERROR type=\"invalid_ts\"/>\n");
            break;
        case PRINT_TEXT:
            printf("lost TS sync");
            break;
        default:
            break;
        }

        block_Delete( p_ts );
        return;
    }

    if ( i_pid != PADDING_PID && p_pids[i_pid].i_last_cc != -1
          && !ts_check_duplicate( i_cc, p_pids[i_pid].i_last_cc )
          && ts_check_discontinuity( i_cc, p_pids[i_pid].i_last_cc ) )
    {
        msg_Warn( NULL, "TS discontinuity" );
        switch ( i_print_type )
        {
        case PRINT_XML:
            printf("<ERROR type=\"invalid_discontinuity\" pid=\"%hu\"/>\n",
                   i_pid);
            break;
        case PRINT_TEXT:
            printf("TS discontinuity (PID=%hu)", i_pid);
            break;
        default:
            break;
        }
    }

    if ( ts_get_transporterror( p_ts->p_ts ) )
    {
        msg_Warn( NULL, "transport_error_indicator" );
        switch ( i_print_type )
        {
        case PRINT_XML:
            printf("<ERROR type=\"transport_error\" pid=\"%hu\"/>\n",
                   i_pid);
            break;
        case PRINT_TEXT:
            printf("transport_error_indicator (PID=%hu)", i_pid);
            break;
        default:
            break;
        }

        i_nb_errors++;
        i_last_error = i_wallclock;
    }
    else if ( i_wallclock > i_last_error + WATCHDOG_WAIT )
        i_nb_errors = 0;

    if ( i_nb_errors > MAX_ERRORS )
    {
        i_nb_errors = 0;
        msg_Warn( NULL,
                 "too many transport errors, tuning again" );
        pf_Reset();
    }

    if ( !ts_get_transporterror( p_ts->p_ts ) )
    {
        /* PSI parsing */
        if ( i_pid == TDT_PID || i_pid == RST_PID )
            SendTDT( p_ts );
        else if ( p_pids[i_pid].i_psi_refcount )
            HandlePSIPacket( p_ts->p_ts, p_ts->i_dts );

        /* PCR handling */
        if ( ts_has_adaptation( p_ts->p_ts )
              && ts_get_adaptation( p_ts->p_ts )
              && tsaf_has_pcr( p_ts->p_ts ) )
        {
            mtime_t i_timestamp = tsaf_get_pcr( p_ts->p_ts );
            int j;

            for ( j = 0; j < i_nb_sids; j++ )
            {
                sid_t *p_sid = pp_sids[j];
                if ( p_sid->i_sid && p_sid->p_current_pmt != NULL
                      && pmt_get_pcrpid( p_sid->p_current_pmt ) == i_pid )
                {
                    for ( i = 0; i < i_nb_outputs; i++ )
                    {
                        output_t *p_output = pp_outputs[i];
                        if ( p_output->config.i_sid == p_sid->i_sid )
                        {
                            p_output->i_ref_timestamp = i_timestamp;
                            p_output->i_ref_wallclock = p_ts->i_dts;
                        }
                    }
                }
            }
        }
    }

    p_pids[i_pid].i_last_cc = i_cc;

    if ( b_enable_emm ) {
        for ( i = 0; i < i_nb_outputs; i++ )
        {
            output_t *p_output = pp_outputs[i];
            if ( p_output->config.i_config & OUTPUT_VALID && p_pids[i_pid].b_emm )
                output_Put( p_output, p_ts );
        }
    }

    /* Output */
    for ( i = 0; i < p_pids[i_pid].i_nb_outputs; i++ )
    {
        output_t *p_output = p_pids[i_pid].pp_outputs[i];
        if ( p_output != NULL )
        {
            if ( i_ca_handle && (p_output->config.i_config & OUTPUT_WATCH) &&
                 ts_get_unitstart( p_ts->p_ts ) )
            {
                uint8_t *p_payload;

                if ( ts_get_scrambling( p_ts->p_ts ) ||
                     ( p_pids[i_pid].b_pes
                        && (p_payload = ts_payload( p_ts->p_ts )) + 3
                             < p_ts->p_ts + TS_SIZE
                          && !pes_validate(p_payload) ) )
                {
                    p_output->i_nb_errors++;
                    p_output->i_last_error = i_wallclock;
                }
                else if ( i_wallclock > p_output->i_last_error + WATCHDOG_WAIT )
                    p_output->i_nb_errors = 0;

                if ( p_output->i_nb_errors > MAX_ERRORS )
                {
                    int j;
                    for ( j = 0; j < i_nb_outputs; j++ )
                        pp_outputs[j]->i_nb_errors = 0;

                    msg_Warn( NULL,
                             "too many errors for stream %s, resetting",
                             p_output->config.psz_displayname );
                    en50221_Reset();
                }
            }

            output_Put( p_output, p_ts );

            if ( p_output->p_eit_ts_buffer != NULL
                  && p_ts->i_dts > p_output->p_eit_ts_buffer->i_dts
                                    + MAX_EIT_RETENTION )
                FlushEIT( p_output, p_ts->i_dts );
        }
    }

    if ( output_dup.config.i_config & OUTPUT_VALID )
        output_Put( &output_dup, p_ts );

    p_ts->i_refcount--;
    if ( !p_ts->i_refcount )
        block_Delete( p_ts );
}

/*****************************************************************************
 * demux_Change : called from main thread
 *****************************************************************************/
static int IsIn( uint16_t *pi_pids, int i_nb_pids, uint16_t i_pid )
{
    int i;
    for ( i = 0; i < i_nb_pids; i++ )
        if ( i_pid == pi_pids[i] ) break;
    return ( i != i_nb_pids );
}

void demux_Change( output_t *p_output, const output_config_t *p_config )
{
    uint16_t *pi_wanted_pids, *pi_current_pids;
    int i_nb_wanted_pids, i_nb_current_pids;

    uint16_t i_old_sid = p_output->config.i_sid;
    uint16_t i_sid = p_config->i_sid;
    uint16_t *pi_old_pids = p_output->config.pi_pids;
    uint16_t *pi_pids = p_config->pi_pids;
    int i_old_nb_pids = p_output->config.i_nb_pids;
    int i_nb_pids = p_config->i_nb_pids;

    bool b_sid_change = i_sid != i_old_sid;
    bool b_pid_change = false, b_tsid_change = false;
    bool b_dvb_change = !!((p_output->config.i_config ^ p_config->i_config)
                             & OUTPUT_DVB);
    int i;

    p_output->config.i_config = p_config->i_config;

    if ( p_config->i_tsid != -1 && p_output->config.i_tsid != p_config->i_tsid )
    {
        p_output->i_tsid = p_config->i_tsid;
        b_tsid_change = true;
    }
    if ( p_config->i_tsid == -1 && p_output->config.i_tsid != -1 )
    {
        if ( psi_table_validate(pp_current_pat_sections) && !b_random_tsid )
            p_output->i_tsid =
                psi_table_get_tableidext(pp_current_pat_sections);
        else
            p_output->i_tsid = rand() & 0xffff;
        b_tsid_change = true;
    }
    p_output->config.i_tsid = p_config->i_tsid;

    if ( !b_sid_change && p_config->i_nb_pids == p_output->config.i_nb_pids &&
         (!p_config->i_nb_pids ||
          !memcmp( p_output->config.pi_pids, p_config->pi_pids,
                   p_config->i_nb_pids * sizeof(uint16_t) )) )
        goto out_change;

    GetPIDS( &pi_wanted_pids, &i_nb_wanted_pids, i_sid, pi_pids, i_nb_pids );
    GetPIDS( &pi_current_pids, &i_nb_current_pids, i_old_sid, pi_old_pids,
             i_old_nb_pids );

    if ( b_sid_change && i_old_sid )
    {
        p_output->config.i_sid = p_config->i_sid;
        for ( i = 0; i < i_nb_sids; i++ )
        {
            if ( pp_sids[i]->i_sid == i_old_sid )
            {
                UnsetPID( pp_sids[i]->i_pmt_pid );

                if ( i_ca_handle && !SIDIsSelected( i_old_sid )
                      && pp_sids[i]->p_current_pmt != NULL
                      && PMTNeedsDescrambling( pp_sids[i]->p_current_pmt ) )
                    en50221_DeletePMT( pp_sids[i]->p_current_pmt );
                break;
            }
        }
    }

    for ( i = 0; i < i_nb_current_pids; i++ )
    {
        if ( !IsIn( pi_wanted_pids, i_nb_wanted_pids, pi_current_pids[i] ) )
        {
            StopPID( p_output, pi_current_pids[i] );
            b_pid_change = true;
        }
    }

    if ( b_sid_change && i_ca_handle && i_old_sid &&
         SIDIsSelected( i_old_sid ) )
    {
        for ( i = 0; i < i_nb_sids; i++ )
        {
            if ( pp_sids[i]->i_sid == i_old_sid )
            {
                if ( pp_sids[i]->p_current_pmt != NULL
                      && PMTNeedsDescrambling( pp_sids[i]->p_current_pmt ) )
                    en50221_UpdatePMT( pp_sids[i]->p_current_pmt );
                break;
            }
        }
    }

    for ( i = 0; i < i_nb_wanted_pids; i++ )
    {
        if ( !IsIn( pi_current_pids, i_nb_current_pids, pi_wanted_pids[i] ) )
        {
            StartPID( p_output, pi_wanted_pids[i] );
            b_pid_change = true;
        }
    }

    free( pi_wanted_pids );
    free( pi_current_pids );

    if ( b_sid_change && i_sid )
    {
        p_output->config.i_sid = i_old_sid;
        for ( i = 0; i < i_nb_sids; i++ )
        {
            if ( pp_sids[i]->i_sid == i_sid )
            {
                SetPID( pp_sids[i]->i_pmt_pid );

                if ( i_ca_handle && !SIDIsSelected( i_sid )
                      && pp_sids[i]->p_current_pmt != NULL
                      && PMTNeedsDescrambling( pp_sids[i]->p_current_pmt ) )
                    en50221_AddPMT( pp_sids[i]->p_current_pmt );
                break;
            }
        }
    }

    if ( i_ca_handle && i_sid && SIDIsSelected( i_sid ) )
    {
        for ( i = 0; i < i_nb_sids; i++ )
        {
            if ( pp_sids[i]->i_sid == i_sid )
            {
                if ( pp_sids[i]->p_current_pmt != NULL
                      && PMTNeedsDescrambling( pp_sids[i]->p_current_pmt ) )
                    en50221_UpdatePMT( pp_sids[i]->p_current_pmt );
                break;
            }
        }
    }

    p_output->config.i_sid = i_sid;
    free( p_output->config.pi_pids );
    p_output->config.pi_pids = malloc( sizeof(uint16_t) * i_nb_pids );
    memcpy( p_output->config.pi_pids, pi_pids, sizeof(uint16_t) * i_nb_pids );
    p_output->config.i_nb_pids = i_nb_pids;

out_change:
    if ( b_sid_change )
    {
        NewSDT( p_output );
        NewNIT( p_output );
        NewPAT( p_output );
        NewPMT( p_output );
    }
    else
    {
        if ( b_tsid_change )
        {
            NewSDT( p_output );
            NewNIT( p_output );
            NewPAT( p_output );
        }
        else if ( b_dvb_change )
        {
            NewNIT( p_output );
            NewPAT( p_output );
        }

        if ( b_pid_change )
            NewPMT( p_output );
    }
}

/*****************************************************************************
 * SetDTS
 *****************************************************************************/
static void SetDTS( block_t *p_list )
{
    int i_nb_ts = 0, i;
    mtime_t i_duration;
    block_t *p_ts = p_list;

    while ( p_ts != NULL )
    {
        i_nb_ts++;
        p_ts = p_ts->p_next;
    }

    /* We suppose the stream is CBR, at least between two consecutive read().
     * This is especially true in budget mode */
    if ( i_last_dts == -1 )
        i_duration = 0;
    else
        i_duration = i_wallclock - i_last_dts;

    p_ts = p_list;
    i = i_nb_ts - 1;
    while ( p_ts != NULL )
    {
        p_ts->i_dts = i_wallclock - i_duration * i / i_nb_ts;
        i--;
        p_ts = p_ts->p_next;
    }

    i_last_dts = i_wallclock;
}

/*****************************************************************************
 * SetPID/UnsetPID
 *****************************************************************************/
static void SetPID( uint16_t i_pid )
{
    p_pids[i_pid].i_refcount++;

    if ( !b_budget_mode && p_pids[i_pid].i_refcount
          && p_pids[i_pid].i_demux_fd == -1 )
        p_pids[i_pid].i_demux_fd = pf_SetFilter( i_pid );
}

static void SetPID_EMM( uint16_t i_pid )
{
    SetPID( i_pid );
    p_pids[i_pid].b_emm = true;
}

static void UnsetPID( uint16_t i_pid )
{
    p_pids[i_pid].i_refcount--;
    p_pids[i_pid].b_emm = false;

    if ( !b_budget_mode && !p_pids[i_pid].i_refcount
          && p_pids[i_pid].i_demux_fd != -1 )
    {
        pf_UnsetFilter( p_pids[i_pid].i_demux_fd, i_pid );
        p_pids[i_pid].i_demux_fd = -1;
    }
}

/*****************************************************************************
 * StartPID/StopPID
 *****************************************************************************/
static void StartPID( output_t *p_output, uint16_t i_pid )
{
    int j;

    for ( j = 0; j < p_pids[i_pid].i_nb_outputs; j++ )
        if ( p_pids[i_pid].pp_outputs[j] == p_output )
            break;

    if ( j == p_pids[i_pid].i_nb_outputs )
    {
        for ( j = 0; j < p_pids[i_pid].i_nb_outputs; j++ )
            if ( p_pids[i_pid].pp_outputs[j] == NULL )
                break;

        if ( j == p_pids[i_pid].i_nb_outputs )
        {
            p_pids[i_pid].i_nb_outputs++;
            p_pids[i_pid].pp_outputs = realloc( p_pids[i_pid].pp_outputs,
                                                sizeof(output_t *)
                                                * p_pids[i_pid].i_nb_outputs );
        }

        p_pids[i_pid].pp_outputs[j] = p_output;
        SetPID( i_pid );
    }
}

static void StopPID( output_t *p_output, uint16_t i_pid )
{
    int j;

    for ( j = 0; j < p_pids[i_pid].i_nb_outputs; j++ )
    {
        if ( p_pids[i_pid].pp_outputs[j] != NULL )
        {
            if ( p_pids[i_pid].pp_outputs[j] == p_output )
                break;
        }
    }

    if ( j != p_pids[i_pid].i_nb_outputs )
    {
        p_pids[i_pid].pp_outputs[j] = NULL;
        UnsetPID( i_pid );
    }
}

/*****************************************************************************
 * SelectPID/UnselectPID
 *****************************************************************************/
static void SelectPID( uint16_t i_sid, uint16_t i_pid )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
        if ( (pp_outputs[i]->config.i_config & OUTPUT_VALID)
              && pp_outputs[i]->config.i_sid == i_sid
              && !pp_outputs[i]->config.i_nb_pids )
            StartPID( pp_outputs[i], i_pid );
}

static void UnselectPID( uint16_t i_sid, uint16_t i_pid )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
        if ( (pp_outputs[i]->config.i_config & OUTPUT_VALID)
              && pp_outputs[i]->config.i_sid == i_sid
              && !pp_outputs[i]->config.i_nb_pids )
            StopPID( pp_outputs[i], i_pid );
}

/*****************************************************************************
 * SelectPMT/UnselectPMT
 *****************************************************************************/
static void SelectPMT( uint16_t i_sid, uint16_t i_pid )
{
    int i;

    p_pids[i_pid].i_psi_refcount++;
    p_pids[i_pid].b_pes = false;

    if ( b_select_pmts )
        SetPID( i_pid );
    else for ( i = 0; i < i_nb_outputs; i++ )
        if ( (pp_outputs[i]->config.i_config & OUTPUT_VALID)
              && pp_outputs[i]->config.i_sid == i_sid )
            SetPID( i_pid );
}

static void UnselectPMT( uint16_t i_sid, uint16_t i_pid )
{
    int i;

    p_pids[i_pid].i_psi_refcount--;
    if ( !p_pids[i_pid].i_psi_refcount )
        psi_assemble_reset( &p_pids[i_pid].p_psi_buffer,
                            &p_pids[i_pid].i_psi_buffer_used );

    if ( b_select_pmts )
        UnsetPID( i_pid );
    else for ( i = 0; i < i_nb_outputs; i++ )
        if ( (pp_outputs[i]->config.i_config & OUTPUT_VALID)
              && pp_outputs[i]->config.i_sid == i_sid )
            UnsetPID( i_pid );
}

/*****************************************************************************
 * GetPIDS
 *****************************************************************************/
static void GetPIDS( uint16_t **ppi_wanted_pids, int *pi_nb_wanted_pids,
                     uint16_t i_sid,
                     const uint16_t *pi_pids, int i_nb_pids )
{
    uint8_t *p_pmt = NULL;
    uint16_t i_pmt_pid, i_pcr_pid;
    uint8_t *p_es;
    int i;
    uint8_t j;

    if ( i_nb_pids || i_sid == 0 )
    {
        *pi_nb_wanted_pids = i_nb_pids;
        *ppi_wanted_pids = malloc( sizeof(uint16_t) * i_nb_pids );
        memcpy( *ppi_wanted_pids, pi_pids, sizeof(uint16_t) * i_nb_pids );
        return;
    }

    *pi_nb_wanted_pids = 0;
    *ppi_wanted_pids = NULL;

    for ( i = 0; i < i_nb_sids; i++ )
    {
        if ( pp_sids[i]->i_sid == i_sid )
        {
            p_pmt = pp_sids[i]->p_current_pmt;
            i_pmt_pid = pp_sids[i]->i_pmt_pid;
            break;
        }
    }

    if ( p_pmt == NULL )
        return;

    i_pcr_pid = pmt_get_pcrpid( p_pmt );
    j = 0;
    while ( (p_es = pmt_get_es( p_pmt, j )) != NULL )
    {
        j++;
        if ( PIDWouldBeSelected( p_es ) )
        {
            *ppi_wanted_pids = realloc( *ppi_wanted_pids,
                                  (*pi_nb_wanted_pids + 1) * sizeof(uint16_t) );
            (*ppi_wanted_pids)[(*pi_nb_wanted_pids)++] = pmtn_get_pid( p_es );
        }
    }

    if ( i_pcr_pid != PADDING_PID && i_pcr_pid != i_pmt_pid
          && !IsIn( *ppi_wanted_pids, *pi_nb_wanted_pids, i_pcr_pid ) )
    {
        *ppi_wanted_pids = realloc( *ppi_wanted_pids,
                              (*pi_nb_wanted_pids + 1) * sizeof(uint16_t) );
        (*ppi_wanted_pids)[(*pi_nb_wanted_pids)++] = i_pcr_pid;
    }
}

/*****************************************************************************
 * OutputPSISection
 *****************************************************************************/
static void OutputPSISection( output_t *p_output, uint8_t *p_section,
                              uint16_t i_pid, uint8_t *pi_cc, mtime_t i_dts,
                              block_t **pp_ts_buffer,
                              uint8_t *pi_ts_buffer_offset )
{
    uint16_t i_section_length = psi_get_length(p_section) + PSI_HEADER_SIZE;
    uint16_t i_section_offset = 0;

    do
    {
        block_t *p_block;
        uint8_t *p;
        uint8_t i_ts_offset;
        bool b_append = (pp_ts_buffer != NULL && *pp_ts_buffer != NULL);

        if ( b_append )
        {
            p_block = *pp_ts_buffer;
            i_ts_offset = *pi_ts_buffer_offset;
        }
        else
        {
            p_block = block_New();
            p_block->i_dts = i_dts;
            i_ts_offset = 0;
        }
        p = p_block->p_ts;

        psi_split_section( p, &i_ts_offset, p_section, &i_section_offset );

        if ( !b_append )
        {
            ts_set_pid( p, i_pid );
            ts_set_cc( p, *pi_cc );
            (*pi_cc)++;
            *pi_cc &= 0xf;
        }

        if ( i_section_offset == i_section_length )
        {
            if ( i_ts_offset < TS_SIZE - MIN_SECTION_FRAGMENT
                  && pp_ts_buffer != NULL )
            {
                *pp_ts_buffer = p_block;
                *pi_ts_buffer_offset = i_ts_offset;
                break;
            }
            else
                psi_split_end( p, &i_ts_offset );
        }

        p_block->i_dts = i_dts;
        p_block->i_refcount--;
        output_Put( p_output, p_block );
        if ( pp_ts_buffer != NULL )
        {
            *pp_ts_buffer = NULL;
            *pi_ts_buffer_offset = 0;
        }
    }
    while ( i_section_offset < i_section_length );
}

/*****************************************************************************
 * SendPAT
 *****************************************************************************/
static void SendPAT( mtime_t i_dts )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( !(p_output->config.i_config & OUTPUT_VALID) )
            continue;

        if ( p_output->p_pat_section == NULL &&
             psi_table_validate(pp_current_pat_sections) )
        {
            /* SID doesn't exist - build an empty PAT. */
            uint8_t *p;
            p_output->i_pat_version++;

            p = p_output->p_pat_section = psi_allocate();
            pat_init( p );
            pat_set_length( p, 0 );
            pat_set_tsid( p, p_output->i_tsid );
            psi_set_version( p, p_output->i_pat_version );
            psi_set_current( p );
            psi_set_section( p, 0 );
            psi_set_lastsection( p, 0 );
            psi_set_crc( p_output->p_pat_section );
        }


        if ( p_output->p_pat_section != NULL )
            OutputPSISection( p_output, p_output->p_pat_section, PAT_PID,
                              &p_output->i_pat_cc, i_dts, NULL, NULL );
    }
}

/*****************************************************************************
 * SendPMT
 *****************************************************************************/
static void SendPMT( sid_t *p_sid, mtime_t i_dts )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( (p_output->config.i_config & OUTPUT_VALID)
               && p_output->config.i_sid == p_sid->i_sid
               && p_output->p_pmt_section != NULL )
            OutputPSISection( p_output, p_output->p_pmt_section,
                              p_sid->i_pmt_pid, &p_output->i_pmt_cc, i_dts,
                              NULL, NULL );
    }
}

/*****************************************************************************
 * SendNIT
 *****************************************************************************/
static void SendNIT( mtime_t i_dts )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( (p_output->config.i_config & OUTPUT_VALID)
               && (p_output->config.i_config & OUTPUT_DVB)
               && p_output->p_nit_section != NULL )
            OutputPSISection( p_output, p_output->p_nit_section, NIT_PID,
                              &p_output->i_nit_cc, i_dts, NULL, NULL );
    }
}

/*****************************************************************************
 * SendSDT
 *****************************************************************************/
static void SendSDT( mtime_t i_dts )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( (p_output->config.i_config & OUTPUT_VALID)
               && (p_output->config.i_config & OUTPUT_DVB)
               && p_output->p_sdt_section != NULL )
            OutputPSISection( p_output, p_output->p_sdt_section, SDT_PID,
                              &p_output->i_sdt_cc, i_dts, NULL, NULL );
    }
}

/*****************************************************************************
 * SendEIT
 *****************************************************************************/
static void SendEIT( sid_t *p_sid, mtime_t i_dts, uint8_t *p_eit )
{
    uint8_t i_table_id = psi_get_tableid( p_eit );
    bool b_epg = i_table_id >= EIT_TABLE_ID_SCHED_ACTUAL_FIRST &&
                 i_table_id <= EIT_TABLE_ID_SCHED_ACTUAL_LAST;
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( (p_output->config.i_config & OUTPUT_VALID)
               && (p_output->config.i_config & OUTPUT_DVB)
               && (!b_epg || (p_output->config.i_config & OUTPUT_EPG))
               && p_output->config.i_sid == p_sid->i_sid )
        {
            if ( eit_get_tsid( p_eit ) != p_output->i_tsid )
            {
                eit_set_tsid( p_eit, p_output->i_tsid );
                psi_set_crc( p_eit );
            }

            OutputPSISection( p_output, p_eit, EIT_PID, &p_output->i_eit_cc,
                              i_dts, &p_output->p_eit_ts_buffer,
                              &p_output->i_eit_ts_buffer_offset );
        }
    }
}

/*****************************************************************************
 * FlushEIT
 *****************************************************************************/
static void FlushEIT( output_t *p_output, mtime_t i_dts )
{
    block_t *p_block = p_output->p_eit_ts_buffer;

    psi_split_end( p_block->p_ts, &p_output->i_eit_ts_buffer_offset );
    p_block->i_dts = i_dts;
    p_block->i_refcount--;
    output_Put( p_output, p_block );
    p_output->p_eit_ts_buffer = NULL;
    p_output->i_eit_ts_buffer_offset = 0;
}

/*****************************************************************************
 * SendTDT
 *****************************************************************************/
static void SendTDT( block_t *p_ts )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( (p_output->config.i_config & OUTPUT_VALID)
               && (p_output->config.i_config & OUTPUT_DVB)
               && p_output->p_sdt_section != NULL )
            output_Put( p_output, p_ts );
    }
}

/*****************************************************************************
 * NewPAT
 *****************************************************************************/
static void NewPAT( output_t *p_output )
{
    const uint8_t *p_program;
    uint8_t *p;
    uint8_t k = 0;

    free( p_output->p_pat_section );
    p_output->p_pat_section = NULL;
    p_output->i_pat_version++;

    if ( !p_output->config.i_sid ) return;
    if ( !psi_table_validate(pp_current_pat_sections) ) return;

    p_program = pat_table_find_program( pp_current_pat_sections,
                                        p_output->config.i_sid );
    if ( p_program == NULL ) return;

    p = p_output->p_pat_section = psi_allocate();
    pat_init( p );
    psi_set_length( p, PSI_MAX_SIZE );
    pat_set_tsid( p, p_output->i_tsid );
    psi_set_version( p, p_output->i_pat_version );
    psi_set_current( p );
    psi_set_section( p, 0 );
    psi_set_lastsection( p, 0 );

    if ( p_output->config.i_config & OUTPUT_DVB )
    {
        /* NIT */
        p = pat_get_program( p_output->p_pat_section, k++ );
        patn_init( p );
        patn_set_program( p, 0 );
        patn_set_pid( p, NIT_PID );
    }

    p = pat_get_program( p_output->p_pat_section, k++ );
    patn_init( p );
    patn_set_program( p, p_output->config.i_sid );
    patn_set_pid( p, patn_get_pid( p_program ) );

    p = pat_get_program( p_output->p_pat_section, k );
    pat_set_length( p_output->p_pat_section,
                    p - p_output->p_pat_section - PAT_HEADER_SIZE );
    psi_set_crc( p_output->p_pat_section );
}

/*****************************************************************************
 * NewPMT
 *****************************************************************************/
static void CopyDescriptors( uint8_t *p_descs, uint8_t *p_current_descs )
{
    uint8_t *p_desc;
    const uint8_t *p_current_desc;
    uint16_t j = 0, k = 0;

    descs_set_length( p_descs, DESCS_MAX_SIZE );

    while ( (p_current_desc = descs_get_desc( p_current_descs, j )) != NULL )
    {
        uint8_t i_tag = desc_get_tag( p_current_desc );

        j++;
        /* A descrambled stream is not supposed to carry CA descriptors. */
        if ( i_ca_handle && i_tag == 0x9 ) continue;

        p_desc = descs_get_desc( p_descs, k );
        if ( p_desc == NULL ) continue; /* This shouldn't happen */
        k++;
        memcpy( p_desc, p_current_desc,
                DESC_HEADER_SIZE + desc_get_length( p_current_desc ) );
    }

    p_desc = descs_get_desc( p_descs, k );
    if ( p_desc == NULL )
        /* This shouldn't happen if the incoming PMT is valid */
        descs_set_length( p_descs, 0 );
    else
        descs_set_length( p_descs, p_desc - p_descs - DESCS_HEADER_SIZE );
}

static void NewPMT( output_t *p_output )
{
    uint8_t *p_current_pmt;
    uint8_t *p_es, *p_current_es;
    uint8_t *p;
    int i;
    uint16_t j, k;

    free( p_output->p_pmt_section );
    p_output->p_pmt_section = NULL;
    p_output->i_pmt_version++;

    if ( !p_output->config.i_sid ) return;

    for ( i = 0; i < i_nb_sids; i++ )
        if ( pp_sids[i]->i_sid == p_output->config.i_sid )
            break;

    if ( i == i_nb_sids ) return;

    if ( pp_sids[i]->p_current_pmt == NULL ) return;
    p_current_pmt = pp_sids[i]->p_current_pmt;

    p = p_output->p_pmt_section = psi_allocate();
    pmt_init( p );
    psi_set_length( p, PSI_MAX_SIZE );
    pmt_set_program( p, p_output->config.i_sid );
    psi_set_version( p, p_output->i_pmt_version );
    psi_set_current( p );
    pmt_set_pcrpid( p, pmt_get_pcrpid( p_current_pmt ) );
    pmt_set_desclength( p, 0 );

    CopyDescriptors( pmt_get_descs( p ), pmt_get_descs( p_current_pmt ) );

    j = 0; k = 0;
    while ( (p_current_es = pmt_get_es( p_current_pmt, j )) != NULL )
    {
        uint16_t i_pid = pmtn_get_pid( p_current_es );

        j++;
        if ( (p_output->config.i_nb_pids || !PIDWouldBeSelected( p_current_es ))
              && !IsIn( p_output->config.pi_pids, p_output->config.i_nb_pids,
                        i_pid ) )
            continue;

        p_es = pmt_get_es( p, k );
        if ( p_es == NULL ) continue; /* This shouldn't happen */
        k++;
        pmtn_init( p_es );
        pmtn_set_streamtype( p_es, pmtn_get_streamtype( p_current_es ) );
        pmtn_set_pid( p_es, i_pid );
        pmtn_set_desclength( p_es, 0 );

        CopyDescriptors( pmtn_get_descs( p_es ),
                         pmtn_get_descs( p_current_es ) );
    }

    p_es = pmt_get_es( p, k );
    if ( p_es == NULL )
        /* This shouldn't happen if the incoming PMT is valid */
        pmt_set_length( p, 0 );
    else
        pmt_set_length( p, p_es - p - PMT_HEADER_SIZE );
    psi_set_crc( p );
}

/*****************************************************************************
 * NewNIT
 *****************************************************************************/
static void NewNIT( output_t *p_output )
{
    uint8_t *p_ts;
    uint8_t *p_header2;
    uint8_t *p;

    free( p_output->p_nit_section );
    p_output->p_nit_section = NULL;
    p_output->i_nit_version++;

    p = p_output->p_nit_section = psi_allocate();
    nit_init( p, true );
    nit_set_length( p, PSI_MAX_SIZE );
    nit_set_nid( p, i_network_id );
    psi_set_version( p, p_output->i_nit_version );
    psi_set_current( p );
    psi_set_section( p, 0 );
    psi_set_lastsection( p, 0 );

    if ( p_network_name != NULL )
    {
        uint8_t *p_descs;
        uint8_t *p_desc;
        nit_set_desclength( p, DESCS_MAX_SIZE );
        p_descs = nit_get_descs( p );
        p_desc = descs_get_desc( p_descs, 0 );
        desc40_init( p_desc );
        desc40_set_networkname( p_desc, p_network_name, i_network_name_size );
        p_desc = descs_get_desc( p_descs, 1 );
        descs_set_length( p_descs, p_desc - p_descs - DESCS_HEADER_SIZE );
    }
    else
        nit_set_desclength( p, 0 );

    p_header2 = nit_get_header2( p );
    nith_init( p_header2 );
    nith_set_tslength( p_header2, NIT_TS_SIZE );

    p_ts = nit_get_ts( p, 0 );
    nitn_init( p_ts );
    nitn_set_tsid( p_ts, p_output->i_tsid );
    nitn_set_onid( p_ts, i_network_id );
    nitn_set_desclength( p_ts, 0 );

    p_ts = nit_get_ts( p, 1 );
    if ( p_ts == NULL )
        /* This shouldn't happen */
        nit_set_length( p, 0 );
    else
        nit_set_length( p, p_ts - p - NIT_HEADER_SIZE );
    psi_set_crc( p_output->p_nit_section );
}

/*****************************************************************************
 * NewSDT
 *****************************************************************************/
static void NewSDT( output_t *p_output )
{
    uint8_t *p_service, *p_current_service;
    uint8_t *p;

    free( p_output->p_sdt_section );
    p_output->p_sdt_section = NULL;
    p_output->i_sdt_version++;

    if ( !p_output->config.i_sid ) return;
    if ( !psi_table_validate(pp_current_sdt_sections) ) return;

    p_current_service = sdt_table_find_service( pp_current_sdt_sections,
                                                p_output->config.i_sid );

    if ( p_current_service == NULL )
    {
        if ( p_output->p_pat_section != NULL &&
             pat_get_program( p_output->p_pat_section, 0 ) == NULL )
        {
            /* Empty PAT and no SDT anymore */
            free( p_output->p_pat_section );
            p_output->p_pat_section = NULL;
            p_output->i_pat_version++;
        }
        return;
    }

    p = p_output->p_sdt_section = psi_allocate();
    sdt_init( p, true );
    sdt_set_length( p, PSI_MAX_SIZE );
    sdt_set_tsid( p, p_output->i_tsid );
    psi_set_version( p, p_output->i_sdt_version );
    psi_set_current( p );
    psi_set_section( p, 0 );
    psi_set_lastsection( p, 0 );
    sdt_set_onid( p,
        sdt_get_onid( psi_table_get_section( pp_current_sdt_sections, 0 ) ) );

    p_service = sdt_get_service( p, 0 );
    sdtn_init( p_service );
    sdtn_set_sid( p_service, p_output->config.i_sid );
    if ( sdtn_get_eitschedule(p_current_service) )
        sdtn_set_eitschedule(p_service);
    if ( sdtn_get_eitpresent(p_current_service) )
        sdtn_set_eitpresent(p_service);
    sdtn_set_running( p_service, sdtn_get_running(p_current_service) );
    /* Do not set free_ca */
    sdtn_set_desclength( p_service, sdtn_get_desclength(p_current_service) );
    memcpy( descs_get_desc( sdtn_get_descs(p_service), 0 ),
            descs_get_desc( sdtn_get_descs(p_current_service), 0 ),
            sdtn_get_desclength(p_current_service) );

    p_service = sdt_get_service( p, 1 );
    if ( p_service == NULL )
        /* This shouldn't happen if the incoming SDT is valid */
        sdt_set_length( p, 0 );
    else
        sdt_set_length( p, p_service - p - SDT_HEADER_SIZE );
    psi_set_crc( p_output->p_sdt_section );
}

/*****************************************************************************
 * UpdatePAT/PMT/SDT
 *****************************************************************************/
#define DECLARE_UPDATE_FUNC( table )                                        \
static void Update##table( uint16_t i_sid )                                 \
{                                                                           \
    int i;                                                                  \
                                                                            \
    for ( i = 0; i < i_nb_outputs; i++ )                                    \
        if ( ( pp_outputs[i]->config.i_config & OUTPUT_VALID )              \
             && pp_outputs[i]->config.i_sid == i_sid )                      \
            New##table( pp_outputs[i] );                                    \
}

DECLARE_UPDATE_FUNC(PAT)
DECLARE_UPDATE_FUNC(PMT)
DECLARE_UPDATE_FUNC(SDT)

/*****************************************************************************
 * UpdateTSID
 *****************************************************************************/
static void UpdateTSID(void)
{
    uint16_t i_tsid = psi_table_get_tableidext(pp_current_pat_sections);
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
    {
        output_t *p_output = pp_outputs[i];

        if ( (p_output->config.i_config & OUTPUT_VALID)
              && p_output->config.i_tsid == -1 && !b_random_tsid )
        {
            p_output->i_tsid = i_tsid;
            NewNIT( p_output );
        }
    }
}

/*****************************************************************************
 * SIDIsSelected
 *****************************************************************************/
static bool SIDIsSelected( uint16_t i_sid )
{
    int i;

    for ( i = 0; i < i_nb_outputs; i++ )
        if ( ( pp_outputs[i]->config.i_config & OUTPUT_VALID )
             && pp_outputs[i]->config.i_sid == i_sid )
            return true;

    return false;
}

/*****************************************************************************
 * demux_PIDIsSelected
 *****************************************************************************/
bool demux_PIDIsSelected( uint16_t i_pid )
{
    int i;

    for ( i = 0; i < p_pids[i_pid].i_nb_outputs; i++ )
        if ( p_pids[i_pid].pp_outputs[i] != NULL )
            return true;

    return false;
}

/*****************************************************************************
 * PIDWouldBeSelected
 *****************************************************************************/
static bool PIDWouldBeSelected( uint8_t *p_es )
{
    if ( b_any_type ) return true;

    uint8_t i_type = pmtn_get_streamtype( p_es );

    switch ( i_type )
    {
    case 0x1: /* video MPEG-1 */
    case 0x2: /* video */
    case 0x3: /* audio MPEG-1 */
    case 0x4: /* audio */
    case 0xf: /* audio AAC ADTS */
    case 0x10: /* video MPEG-4 */
    case 0x11: /* audio AAC LATM */
    case 0x1b: /* video H264 */
    case 0x81: /* ATSC A/52 */
    case 0x87: /* ATSC Enhanced A/52 */
        return true;
        break;

    case 0x6:
    {
        uint16_t j = 0;
        const uint8_t *p_desc;

        while ( (p_desc = descs_get_desc( pmtn_get_descs( p_es ), j )) != NULL )
        {
            uint8_t i_tag = desc_get_tag( p_desc );
            j++;

            if( i_tag == 0x46 /* VBI + teletext */
                 || i_tag == 0x56 /* teletext */
                 || i_tag == 0x59 /* dvbsub */
                 || i_tag == 0x6a /* A/52 */
                 || i_tag == 0x7a /* Enhanced A/52 */
                 || i_tag == 0x7b /* DCA */
                 || i_tag == 0x7c /* AAC */ )
                return true;
        }
        break;
    }

    default:
        break;
    }

    /* FIXME: also parse IOD */
    return false;
}

/*****************************************************************************
 * PIDCarriesPES
 *****************************************************************************/
static bool PIDCarriesPES( const uint8_t *p_es )
{
    uint8_t i_type = pmtn_get_streamtype( p_es );

    switch ( i_type )
    {
    case 0x1: /* video MPEG-1 */
    case 0x2: /* video */
    case 0x3: /* audio MPEG-1 */
    case 0x4: /* audio */
    case 0x6: /* private PES data */
    case 0xf: /* audio AAC */
    case 0x10: /* video MPEG-4 */
    case 0x11: /* audio AAC LATM */
    case 0x1b: /* video H264 */
    case 0x81: /* ATSC A/52 */
    case 0x87: /* ATSC Enhanced A/52 */
        return true;
        break;

    default:
        return false;
        break;
    }
}

/*****************************************************************************
 * PMTNeedsDescrambling
 *****************************************************************************/
static bool PMTNeedsDescrambling( uint8_t *p_pmt )
{
    uint8_t i;
    uint16_t j;
    uint8_t *p_es;
    const uint8_t *p_desc;

    j = 0;
    while ( (p_desc = descs_get_desc( pmt_get_descs( p_pmt ), j )) != NULL )
    {
        uint8_t i_tag = desc_get_tag( p_desc );
        j++;

        if ( i_tag == 0x9 ) return true;
    }

    i = 0;
    while ( (p_es = pmt_get_es( p_pmt, i )) != NULL )
    {
        i++;
        j = 0;
        while ( (p_desc = descs_get_desc( pmtn_get_descs( p_es ), j )) != NULL )
        {
            uint8_t i_tag = desc_get_tag( p_desc );
            j++;

            if ( i_tag == 0x9 ) return true;
        }
    }

    return false;
}

/*****************************************************************************
 * demux_ResendCAPMTs
 *****************************************************************************/
void demux_ResendCAPMTs( void )
{
    int i;
    for ( i = 0; i < i_nb_sids; i++ )
        if ( pp_sids[i]->p_current_pmt != NULL
              && SIDIsSelected( pp_sids[i]->i_sid )
              && PMTNeedsDescrambling( pp_sids[i]->p_current_pmt ) )
            en50221_AddPMT( pp_sids[i]->p_current_pmt );
}

/* Find CA descriptor that have PID i_ca_pid */
static uint8_t *ca_desc_find( uint8_t *p_descs, uint16_t i_ca_pid )
{
    int j = 0;
    uint8_t *p_desc;

    while ( (p_desc = descs_get_desc( p_descs, j++ )) != NULL ) {
        if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
            continue;
        if ( desc09_get_pid( p_desc ) == i_ca_pid )
            return p_desc;
    }

    return NULL;
}

/*****************************************************************************
 * DeleteProgram
 *****************************************************************************/
static void DeleteProgram( uint16_t i_sid, uint16_t i_pid )
{
    int i_pmt;

    UnselectPMT( i_sid, i_pid );

    for ( i_pmt = 0; i_pmt < i_nb_sids; i_pmt++ )
    {
        sid_t *p_sid = pp_sids[i_pmt];
        if ( p_sid->i_sid == i_sid )
        {
            uint8_t *p_pmt = p_sid->p_current_pmt;

            if ( p_pmt != NULL )
            {
                uint16_t i_pcr_pid = pmt_get_pcrpid( p_pmt );
                uint8_t *p_es;
                uint8_t j;

                if ( i_ca_handle
                     && SIDIsSelected( i_sid )
                     && PMTNeedsDescrambling( p_pmt ) )
                    en50221_DeletePMT( p_pmt );

                if ( i_pcr_pid != PADDING_PID
                      && i_pcr_pid != p_sid->i_pmt_pid )
                    UnselectPID( i_sid, i_pcr_pid );

                if ( b_enable_ecm )
                {
                    j = 0;
                    uint8_t *p_desc;

                    while ((p_desc = descs_get_desc( pmt_get_descs( p_pmt ), j++ )) != NULL)
                    {
                        if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                            continue;
                        UnselectPID( i_sid, desc09_get_pid( p_desc ) );
                    }
                }

                j = 0;
                while ( (p_es = pmt_get_es( p_pmt, j )) != NULL )
                {
                    uint16_t i_pid = pmtn_get_pid( p_es );
                    j++;

                    if ( PIDWouldBeSelected( p_es ) )
                        UnselectPID( i_sid, i_pid );
                }

                free( p_pmt );
                pp_sids[i_pmt]->p_current_pmt = NULL;
            }
            pp_sids[i_pmt]->i_sid = 0;
            pp_sids[i_pmt]->i_pmt_pid = 0;
            break;
        }
    }
}

/*****************************************************************************
 * demux_Iconv
 *****************************************************************************
 * This code is from biTStream's examples and is under the WTFPL (see
 * LICENSE.WTFPL).
 *****************************************************************************/
static char *iconv_append_null(const char *p_string, size_t i_length)
{
    char *psz_string = malloc(i_length + 1);
    memcpy(psz_string, p_string, i_length);
    psz_string[i_length] = '\0';
    return psz_string;
}

char *demux_Iconv(void *_unused, const char *psz_encoding,
                  char *p_string, size_t i_length)
{
#ifdef HAVE_ICONV
    static const char *psz_current_encoding = "";
    static iconv_t iconv_handle = (iconv_t)-1;

    char *psz_string, *p;
    size_t i_out_length;

    if (!strcmp(psz_encoding, psz_native_charset))
        return iconv_append_null(p_string, i_length);

    if (iconv_handle != (iconv_t)-1 &&
        strcmp(psz_encoding, psz_current_encoding)) {
        iconv_close(iconv_handle);
        iconv_handle = (iconv_t)-1;
    }

    if (iconv_handle == (iconv_t)-1)
        iconv_handle = iconv_open(psz_native_charset, psz_encoding);
    if (iconv_handle == (iconv_t)-1) {
        msg_Warn(NULL, "couldn't convert from %s to %s (%m)", psz_encoding,
                psz_native_charset);
        return iconv_append_null(p_string, i_length);
    }

    /* converted strings can be up to six times larger */
    i_out_length = i_length * 6;
    p = psz_string = malloc(i_out_length);
    if (iconv(iconv_handle, &p_string, &i_length, &p, &i_out_length) == -1) {
        msg_Warn(NULL, "couldn't convert from %s to %s (%m)", psz_encoding,
                psz_native_charset);
        free(psz_string);
        return iconv_append_null(p_string, i_length);
    }
    if (i_length)
        msg_Warn(NULL, "partial conversion from %s to %s", psz_encoding,
                psz_native_charset);

    *p = '\0';
    return psz_string;
#else
    return iconv_append_null(p_string, i_length);
#endif
}

/*****************************************************************************
 * demux_Print
 *****************************************************************************
 * This code is from biTStream's examples and is under the WTFPL (see
 * LICENSE.WTFPL).
 *****************************************************************************/
static void demux_Print(void *_unused, const char *psz_format, ...)
{
    char psz_fmt[strlen(psz_format) + 2];
    va_list args;
    va_start(args, psz_format);
    strcpy(psz_fmt, psz_format);
    if ( i_print_type != PRINT_XML )
        strcat(psz_fmt, "\n");
    vprintf(psz_fmt, args);
}

/*****************************************************************************
 * HandlePAT
 *****************************************************************************/
static void HandlePAT( mtime_t i_dts )
{
    bool b_display, b_change = false;
    PSI_TABLE_DECLARE( pp_old_pat_sections );
    uint8_t i_last_section = psi_table_get_lastsection( pp_next_pat_sections );
    uint8_t i;

    if ( psi_table_validate( pp_current_pat_sections ) &&
         psi_table_compare( pp_current_pat_sections, pp_next_pat_sections ) )
    {
        /* Identical PAT. Shortcut. */
        psi_table_free( pp_next_pat_sections );
        psi_table_init( pp_next_pat_sections );
        goto out_pat;
    }

    if ( !pat_table_validate( pp_next_pat_sections ) )
    {
        msg_Warn( NULL, "invalid PAT received" );
        switch (i_print_type) {
        case PRINT_XML:
            printf("<ERROR type=\"invalid_pat\"/>\n");
            break;
        default:
            printf("invalid PAT received\n");
        }
        psi_table_free( pp_next_pat_sections );
        psi_table_init( pp_next_pat_sections );
        goto out_pat;
    }

    b_display = !psi_table_validate( pp_current_pat_sections )
                 || psi_table_get_version( pp_current_pat_sections )
                     != psi_table_get_version( pp_next_pat_sections );

    /* Switch tables. */
    psi_table_copy( pp_old_pat_sections, pp_current_pat_sections );
    psi_table_copy( pp_current_pat_sections, pp_next_pat_sections );
    psi_table_init( pp_next_pat_sections );

    if ( !psi_table_validate( pp_old_pat_sections )
          || psi_table_get_tableidext( pp_current_pat_sections )
              != psi_table_get_tableidext( pp_old_pat_sections ) )
    {
        b_display = b_change = true;
        UpdateTSID();
        /* This will trigger a universal reset of everything. */
    }

    for ( i = 0; i <= i_last_section; i++ )
    {
        uint8_t *p_section =
            psi_table_get_section( pp_current_pat_sections, i );
        const uint8_t *p_program;
        int j = 0;

        while ( (p_program = pat_get_program( p_section, j )) != NULL )
        {
            const uint8_t *p_old_program = NULL;
            uint16_t i_sid = patn_get_program( p_program );
            uint16_t i_pid = patn_get_pid( p_program );
            j++;

            if ( i_sid == 0 )
            {
                if ( i_pid != NIT_PID )
                    msg_Warn( NULL,
                        "NIT is carried on PID %hu which isn't DVB compliant",
                        i_pid );
                continue; /* NIT */
            }

            if ( !psi_table_validate( pp_old_pat_sections )
                  || (p_old_program = pat_table_find_program(
                                       pp_old_pat_sections, i_sid )) == NULL
                  || patn_get_pid( p_old_program ) != i_pid
                  || b_change )
            {
                int i_pmt;
                b_display = true;

                if ( p_old_program != NULL )
                    DeleteProgram( i_sid, patn_get_pid( p_old_program ) );

                SelectPMT( i_sid, i_pid );

                for ( i_pmt = 0; i_pmt < i_nb_sids; i_pmt++ )
                    if ( pp_sids[i_pmt]->i_sid == 0 )
                        break;

                if ( i_pmt == i_nb_sids )
                {
                    sid_t *p_sid = malloc( sizeof(sid_t) );
                    p_sid->p_current_pmt = NULL;
                    i_nb_sids++;
                    pp_sids = realloc( pp_sids, sizeof(sid_t *) * i_nb_sids );
                    pp_sids[i_pmt] = p_sid;
                }

                pp_sids[i_pmt]->i_sid = i_sid;
                pp_sids[i_pmt]->i_pmt_pid = i_pid;

                UpdatePAT( i_sid );
            }
        }
    }

    if ( psi_table_validate( pp_old_pat_sections ) )
    {
        i_last_section = psi_table_get_lastsection( pp_old_pat_sections );
        for ( i = 0; i <= i_last_section; i++ )
        {
            uint8_t *p_section =
                psi_table_get_section( pp_old_pat_sections, i );
            const uint8_t *p_program;
            int j = 0;

            while ( (p_program = pat_get_program( p_section, j )) != NULL )
            {
                uint16_t i_sid = patn_get_program( p_program );
                uint16_t i_pid = patn_get_pid( p_program );
                j++;

                if ( i_sid == 0 )
                    continue; /* NIT */

                if ( pat_table_find_program( pp_current_pat_sections, i_sid )
                      == NULL )
                {
                    b_display = true;
                    DeleteProgram( i_sid, i_pid );
                    UpdatePAT( i_sid );
                }
            }
        }

        psi_table_free( pp_old_pat_sections );
    }

    if ( b_display )
    {
        pat_table_print( pp_current_pat_sections, msg_Dbg, NULL, PRINT_TEXT );
        if ( i_print_type != -1 )
        {
            pat_table_print( pp_current_pat_sections, demux_Print, NULL,
                             i_print_type );
            if ( i_print_type == PRINT_XML )
                printf("\n");
        }
    }

out_pat:
    SendPAT( i_dts );
}

/*****************************************************************************
 * HandlePATSection
 *****************************************************************************/
static void HandlePATSection( uint16_t i_pid, uint8_t *p_section,
                              mtime_t i_dts )
{
    if ( i_pid != PAT_PID || !pat_validate( p_section ) )
    {
        msg_Warn( NULL, "invalid PAT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            printf("<ERROR type=\"invalid_pat_section\"/>\n");
            break;
        default:
            printf("invalid PAT section received on PID %hu\n", i_pid);
        }
        free( p_section );
        return;
    }

    if ( !psi_table_section( pp_next_pat_sections, p_section ) )
        return;

    HandlePAT( i_dts );
}

/*****************************************************************************
 * HandleCAT
 *****************************************************************************/
static void HandleCAT( mtime_t i_dts )
{
    bool b_display, b_change = false;
    PSI_TABLE_DECLARE( pp_old_cat_sections );
    uint8_t i_last_section = psi_table_get_lastsection( pp_next_cat_sections );
    uint8_t i_last_section2;
    uint8_t i, r;
    uint8_t *p_desc;
    int j, k;

    if ( psi_table_validate( pp_current_cat_sections ) &&
         psi_table_compare( pp_current_cat_sections, pp_next_cat_sections ) )
    {
        /* Identical CAT. Shortcut. */
        psi_table_free( pp_next_cat_sections );
        psi_table_init( pp_next_cat_sections );
        goto out_cat;
    }

    if ( !cat_table_validate( pp_next_cat_sections ) )
    {
        msg_Warn( NULL, "invalid CAT received" );
        switch (i_print_type) {
        case PRINT_XML:
            printf("<ERROR type=\"invalid_cat\"/>\n");
            break;
        default:
            printf("invalid CAT received\n");
        }
        psi_table_free( pp_next_cat_sections );
        psi_table_init( pp_next_cat_sections );
        goto out_cat;
    }

    b_display = !psi_table_validate( pp_current_cat_sections )
                 || psi_table_get_version( pp_current_cat_sections )
                     != psi_table_get_version( pp_next_cat_sections );

    /* Switch tables. */
    psi_table_copy( pp_old_cat_sections, pp_current_cat_sections );
    psi_table_copy( pp_current_cat_sections, pp_next_cat_sections );
    psi_table_init( pp_next_cat_sections );

    if ( !psi_table_validate( pp_old_cat_sections )
          || psi_table_get_tableidext( pp_current_cat_sections )
              != psi_table_get_tableidext( pp_old_cat_sections ) )
    {
        b_display = b_change = true;
    }

    if ( b_change )
    {
        for ( i = 0; i <= i_last_section; i++ )
        {
            uint8_t *p_section = psi_table_get_section( pp_current_cat_sections, i );

            j = 0;
            uint8_t *p_cat_descs = cat_alloc_descs( p_section );
            while ( (p_desc = descs_get_desc( p_cat_descs, j++ )) != NULL )
            {
                if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                    continue;

                SetPID_EMM( desc09_get_pid( p_desc ) );
            }
            cat_free_descs( p_cat_descs );
        }
    }

    if ( psi_table_validate( pp_old_cat_sections ) )
    {
        i_last_section = psi_table_get_lastsection( pp_old_cat_sections );
        for ( i = 0; i <= i_last_section; i++ )
        {
            uint8_t *p_old_section = psi_table_get_section( pp_old_cat_sections, i );
            j = 0;
            uint8_t *p_old_cat_descs = cat_alloc_descs( p_old_section );
            while ( (p_desc = descs_get_desc( p_old_cat_descs, j++ )) != NULL )
            {
                uint16_t emm_pid;
                int pid_found = 0;

                if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                    continue;

                emm_pid = desc09_get_pid( p_desc );

                // Search in current sections if the pid exists
                i_last_section2 = psi_table_get_lastsection( pp_current_cat_sections );
                for ( r = 0; r <= i_last_section2; r++ )
                {
                    uint8_t *p_section = psi_table_get_section( pp_current_cat_sections, r );

                    k = 0;
                    uint8_t *p_cat_descs = cat_alloc_descs( p_section );
                    while ( (p_desc = descs_get_desc( p_cat_descs, k++ )) != NULL )
                    {
                        if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                            continue;
                        if ( ca_desc_find( p_cat_descs, emm_pid ) != NULL )
                        {
                            pid_found = 1;
                            break;
                        }
                    }
                    cat_free_descs( p_cat_descs );
                }

                if ( !pid_found )
                {
                    UnsetPID(emm_pid);
                    b_display = true;
                }
            }
            cat_free_descs( p_old_cat_descs );
        }

        psi_table_free( pp_old_cat_sections );
    }

    if ( b_display )
    {
        cat_table_print( pp_current_cat_sections, msg_Dbg, NULL, PRINT_TEXT );
        if ( i_print_type != -1 )
        {
            cat_table_print( pp_current_cat_sections, demux_Print, NULL,
                             i_print_type );
            if ( i_print_type == PRINT_XML )
                printf("\n");
        }
    }

out_cat:
    return;
}

/*****************************************************************************
 * HandleCATSection
 *****************************************************************************/
static void HandleCATSection( uint16_t i_pid, uint8_t *p_section,
                              mtime_t i_dts )
{
    if ( i_pid != CAT_PID || !cat_validate( p_section ) )
    {
        msg_Warn( NULL, "invalid CAT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            printf("<ERROR type=\"invalid_cat_section\"/>\n");
            break;
        default:
            printf("invalid CAT section received on PID %hu\n", i_pid);
        }
        free( p_section );
        return;
    }

    if ( !psi_table_section( pp_next_cat_sections, p_section ) )
        return;

    HandleCAT( i_dts );
}

/*****************************************************************************
 * HandlePMT
 *****************************************************************************/
static void HandlePMT( uint16_t i_pid, uint8_t *p_pmt, mtime_t i_dts )
{
    bool b_change, b_new;
    uint16_t i_sid = pmt_get_program( p_pmt );
    sid_t *p_sid;
    bool b_needs_descrambling, b_needed_descrambling, b_is_selected;
    uint16_t i_pcr_pid;
    uint8_t *p_es;
    uint8_t *p_desc;
    int i;
    uint16_t j;

    for ( i = 0; i < i_nb_sids; i++ )
        if ( pp_sids[i]->i_sid && pp_sids[i]->i_sid == i_sid )
            break;

    if ( i == i_nb_sids )
    {
        /* Unwanted SID (happens when the same PMT PID is used for several
         * programs). */
        free( p_pmt );
        return;
    }
    p_sid = pp_sids[i];

    if ( i_pid != p_sid->i_pmt_pid )
    {
        msg_Warn( NULL, "invalid PMT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            printf("<ERROR type=\"ghost_pmt\" program=\"%hu\n pid=\"%hu\"/>\n",
                   i_sid, i_pid);
            break;
        default:
            printf("ghost PMT for service %hu carried on PID %hu\n", i_sid,
                   i_pid);
        }
        free( p_pmt );
        return;
    }

    if ( p_sid->p_current_pmt != NULL &&
         psi_compare( p_sid->p_current_pmt, p_pmt ) )
    {
        /* Identical PMT. Shortcut. */
        free( p_pmt );
        goto out_pmt;
    }

    if ( !pmt_validate( p_pmt ) )
    {
        msg_Warn( NULL, "invalid PMT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            printf("<ERROR type=\"invalid_pmt_section\" pid=\"%hu\"/>\n",
                   i_pid);
            break;
        default:
            printf("invalid PMT section received on PID %hu\n", i_pid);
        }
        free( p_pmt );
        goto out_pmt;
    }

    b_needs_descrambling = PMTNeedsDescrambling( p_pmt );
    b_needed_descrambling = p_sid->p_current_pmt != NULL ?
                            PMTNeedsDescrambling( p_sid->p_current_pmt ) :
                            false;
    b_is_selected = SIDIsSelected( i_sid );
    i_pcr_pid = pmt_get_pcrpid( p_pmt );

    if ( i_ca_handle && b_is_selected &&
         !b_needs_descrambling && b_needed_descrambling )
        en50221_DeletePMT( p_sid->p_current_pmt );

    b_new = b_change = p_sid->p_current_pmt == NULL
                        || psi_get_version( p_sid->p_current_pmt )
                            != psi_get_version( p_pmt );

    if ( b_enable_ecm && b_new ) {
        j = 0;
        while ( (p_desc = descs_get_desc( pmt_get_descs( p_pmt ), j++ )) != NULL )
        {
            if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                continue;
            SelectPID( i_sid, desc09_get_pid( p_desc ) );
        }
    }

    if ( p_sid->p_current_pmt == NULL
          || i_pcr_pid != pmt_get_pcrpid( p_sid->p_current_pmt ) )
    {
        if ( i_pcr_pid != PADDING_PID
              && i_pcr_pid != p_sid->i_pmt_pid )
        {
            b_change = true;
            SelectPID( i_sid, i_pcr_pid );
        }
    }

    j = 0;
    while ( (p_es = pmt_get_es( p_pmt, j )) != NULL )
    {
        uint16_t i_pid = pmtn_get_pid( p_es );
        j++;

        if ( b_new || pmt_find_es( p_sid->p_current_pmt, i_pid ) == NULL )
        {
            b_change = true;
            if ( PIDWouldBeSelected( p_es ) )
                SelectPID( i_sid, i_pid );
            p_pids[i_pid].b_pes = PIDCarriesPES( p_es );
        }
    }

    if ( p_sid->p_current_pmt != NULL )
    {
        if ( b_enable_ecm )
        {
            j = 0;
            while ((p_desc = descs_get_desc( pmt_get_descs( p_sid->p_current_pmt ), j++ )) != NULL)
            {
                if ( desc_get_tag( p_desc ) != 0x09 || !desc09_validate( p_desc ) )
                    continue;
                if ( ca_desc_find( pmt_get_descs( p_pmt ), desc09_get_pid( p_desc ) ) == NULL )
                    UnselectPID( i_sid, desc09_get_pid( p_desc ) );
            }
        }

        uint16_t i_current_pcr_pid = pmt_get_pcrpid( p_sid->p_current_pmt );
        if ( i_current_pcr_pid != i_pcr_pid
              && i_current_pcr_pid != PADDING_PID )
        {
            if ( pmt_find_es( p_pmt, i_current_pcr_pid ) == NULL )
            {
                b_change = true;
                UnselectPID( i_sid, i_current_pcr_pid );
            }
        }

        j = 0;
        while ( (p_es = pmt_get_es( p_sid->p_current_pmt, j )) != NULL )
        {
            j++;

            if ( PIDWouldBeSelected( p_es ) )
            {
                uint16_t i_current_pid = pmtn_get_pid( p_es );

                if ( pmt_find_es( p_pmt, i_current_pid ) == NULL )
                {
                    b_change = true;
                    UnselectPID( i_sid, i_current_pid );
                }
            }
        }

        free( p_sid->p_current_pmt );
    }

    p_sid->p_current_pmt = p_pmt;

    if ( b_change )
    {
        if ( i_ca_handle && b_is_selected )
        {
            if ( b_needs_descrambling && !b_needed_descrambling )
                en50221_AddPMT( p_pmt );
            else if ( b_needs_descrambling && b_needed_descrambling )
                en50221_UpdatePMT( p_pmt );
        }

        UpdatePMT( i_sid );

        pmt_print( p_pmt, msg_Dbg, NULL, demux_Iconv, NULL, PRINT_TEXT );
        if ( i_print_type != -1 )
        {
            pmt_print( p_pmt, demux_Print, NULL, demux_Iconv, NULL,
                       i_print_type );
            if ( i_print_type == PRINT_XML )
                printf("\n");
        }
    }

out_pmt:
    SendPMT( p_sid, i_dts );
}

/*****************************************************************************
 * HandleNIT
 *****************************************************************************/
static void HandleNIT( mtime_t i_dts )
{
    bool b_display;

    if ( psi_table_validate( pp_current_nit_sections ) &&
         psi_table_compare( pp_current_nit_sections, pp_next_nit_sections ) )
    {
        /* Identical NIT. Shortcut. */
        psi_table_free( pp_next_nit_sections );
        psi_table_init( pp_next_nit_sections );
        goto out_nit;
    }

    if ( !nit_table_validate( pp_next_nit_sections ) )
    {
        msg_Warn( NULL, "invalid NIT received" );
        switch (i_print_type) {
        case PRINT_XML:
            printf("<ERROR type=\"invalid_nit\"/>\n");
            break;
        default:
            printf("invalid NIT received\n");
        }
        psi_table_free( pp_next_nit_sections );
        psi_table_init( pp_next_nit_sections );
        goto out_nit;
    }

    b_display = !psi_table_validate( pp_current_nit_sections )
                 || psi_table_get_version( pp_current_nit_sections )
                     != psi_table_get_version( pp_next_nit_sections );

    /* Switch tables. */
    psi_table_free( pp_current_nit_sections );
    psi_table_copy( pp_current_nit_sections, pp_next_nit_sections );
    psi_table_init( pp_next_nit_sections );

    if ( b_display )
    {
        nit_table_print( pp_current_nit_sections, msg_Dbg, NULL,
                         demux_Iconv, NULL, PRINT_TEXT );
        if ( i_print_type != -1 )
        {
            nit_table_print( pp_current_nit_sections, demux_Print, NULL,
                             demux_Iconv, NULL, i_print_type );
            if ( i_print_type == PRINT_XML )
                printf("\n");
        }
    }

out_nit:
    ;
}

/*****************************************************************************
 * HandleNITSection
 *****************************************************************************/
static void HandleNITSection( uint16_t i_pid, uint8_t *p_section,
                              mtime_t i_dts )
{
    if ( i_pid != NIT_PID || !nit_validate( p_section ) )
    {
        msg_Warn( NULL, "invalid NIT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            printf("<ERROR type=\"invalid_nit_section\" pid=\"%hu\"/>\n",
                   i_pid);
            break;
        default:
            printf("invalid NIT section received on PID %hu\n", i_pid);
        }
        free( p_section );
        return;
    }

    if ( psi_table_section( pp_next_nit_sections, p_section ) )
        HandleNIT( i_dts );

    /* This case is different because DVB specifies a minimum bitrate for
     * PID 0x10, even if we don't have any thing to send (for cheap
     * transport over network boundaries). */
    SendNIT( i_dts );
}


/*****************************************************************************
 * HandleSDT
 *****************************************************************************/
static void HandleSDT( mtime_t i_dts )
{
    bool b_change, b_new;
    PSI_TABLE_DECLARE( pp_old_sdt_sections );
    uint8_t i_last_section = psi_table_get_lastsection( pp_next_sdt_sections );
    uint8_t i;
    int j;

    if ( psi_table_validate( pp_current_sdt_sections ) &&
         psi_table_compare( pp_current_sdt_sections, pp_next_sdt_sections ) )
    {
        /* Identical SDT. Shortcut. */
        psi_table_free( pp_next_sdt_sections );
        psi_table_init( pp_next_sdt_sections );
        goto out_sdt;
    }

    if ( !sdt_table_validate( pp_next_sdt_sections ) )
    {
        msg_Warn( NULL, "invalid SDT received" );
        switch (i_print_type) {
        case PRINT_XML:
            printf("<ERROR type=\"invalid_sdt\"/>\n");
            break;
        default:
            printf("invalid SDT received\n");
        }
        psi_table_free( pp_next_sdt_sections );
        psi_table_init( pp_next_sdt_sections );
        goto out_sdt;
    }

    b_change = b_new = !psi_table_validate( pp_current_sdt_sections )
                        || psi_table_get_version( pp_current_sdt_sections )
                            != psi_table_get_version( pp_next_sdt_sections );

    /* Switch tables. */
    psi_table_copy( pp_old_sdt_sections, pp_current_sdt_sections );
    psi_table_copy( pp_current_sdt_sections, pp_next_sdt_sections );
    psi_table_init( pp_next_sdt_sections );

    for ( i = 0; i <= i_last_section; i++ )
    {
        uint8_t *p_section =
            psi_table_get_section( pp_current_sdt_sections, i );
        uint8_t *p_service;
        j = 0;

        while ( (p_service = sdt_get_service( p_section, j )) != NULL )
        {
            uint16_t i_sid = sdtn_get_sid( p_service );
            j++;

            if ( b_new ||
                 sdt_table_find_service( pp_old_sdt_sections, i_sid ) == NULL )
            {
                b_change = true;
                UpdateSDT( i_sid );
            }
        }
    }

    if ( psi_table_validate( pp_old_sdt_sections ) )
    {
        i_last_section = psi_table_get_lastsection( pp_old_sdt_sections );
        for ( i = 0; i <= i_last_section; i++ )
        {
            uint8_t *p_section =
                psi_table_get_section( pp_old_sdt_sections, i );
            const uint8_t *p_service;
            int j = 0;

            while ( (p_service = sdt_get_service( p_section, j )) != NULL )
            {
                uint16_t i_sid = sdtn_get_sid( p_service );
                j++;

                if ( sdt_table_find_service( pp_current_sdt_sections, i_sid )
                      == NULL )
                {
                    b_change = true;
                    UpdateSDT( i_sid );
                }
            }
        }

        psi_table_free( pp_old_sdt_sections );
    }

    if ( b_change )
    {
        sdt_table_print( pp_current_sdt_sections, msg_Dbg, NULL,
                         demux_Iconv, NULL, PRINT_TEXT );
        if ( i_print_type != -1 )
        {
            sdt_table_print( pp_current_sdt_sections, demux_Print, NULL,
                             demux_Iconv, NULL, i_print_type );
            if ( i_print_type == PRINT_XML )
                printf("\n");
        }
    }

out_sdt:
    SendSDT( i_dts );
}

/*****************************************************************************
 * HandleSDTSection
 *****************************************************************************/
static void HandleSDTSection( uint16_t i_pid, uint8_t *p_section,
                              mtime_t i_dts )
{
    if ( i_pid != SDT_PID || !sdt_validate( p_section ) )
    {
        msg_Warn( NULL, "invalid SDT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            printf("<ERROR type=\"invalid_sdt_section\" pid=\"%hu\"/>\n",
                   i_pid);
            break;
        default:
            printf("invalid SDT section received on PID %hu\n", i_pid);
        }
        free( p_section );
        return;
    }

    if ( !psi_table_section( pp_next_sdt_sections, p_section ) )
        return;

    HandleSDT( i_dts );
}

/*****************************************************************************
 * HandleEITSection
 *****************************************************************************/
static void HandleEIT( uint16_t i_pid, uint8_t *p_eit, mtime_t i_dts )
{
    uint16_t i_sid = eit_get_sid( p_eit );
    sid_t *p_sid;
    int i;

    for ( i = 0; i < i_nb_sids; i++ )
        if ( pp_sids[i]->i_sid && pp_sids[i]->i_sid == i_sid )
            break;

    if ( i == i_nb_sids )
    {
        /* Not a selected program. */
        free( p_eit );
        return;
    }
    p_sid = pp_sids[i];

    if ( i_pid != EIT_PID || !eit_validate( p_eit ) )
    {
        msg_Warn( NULL, "invalid EIT section received on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            printf("<ERROR type=\"invalid_eit_section\" pid=\"%hu\"/>\n",
                   i_pid);
            break;
        default:
            printf("invalid EIT section received on PID %hu\n", i_pid);
        }
        free( p_eit );
        return;
    }

    SendEIT( p_sid, i_dts, p_eit );
    free( p_eit );
}

/*****************************************************************************
 * HandleSection
 *****************************************************************************/
static void HandleSection( uint16_t i_pid, uint8_t *p_section, mtime_t i_dts )
{
    uint8_t i_table_id = psi_get_tableid( p_section );

    if ( !psi_validate( p_section ) )
    {
        msg_Warn( NULL, "invalid section on PID %hu", i_pid );
        switch (i_print_type) {
        case PRINT_XML:
            printf("<ERROR type=\"invalid_section\" pid=\"%hu\"/>\n", i_pid);
            break;
        default:
            printf("invalid section on PID %hu\n", i_pid);
        }
        free( p_section );
        return;
    }

    if ( !psi_get_current( p_section ) )
    {
        /* Ignore sections which are not in use yet. */
        free( p_section );
        return;
    }

    switch ( i_table_id )
    {
    case PAT_TABLE_ID:
        HandlePATSection( i_pid, p_section, i_dts );
        break;

    case CAT_TABLE_ID:
        if ( b_enable_emm )
            HandleCATSection( i_pid, p_section, i_dts );
        break;

    case PMT_TABLE_ID:
        HandlePMT( i_pid, p_section, i_dts );
        break;

    case NIT_TABLE_ID_ACTUAL:
        HandleNITSection( i_pid, p_section, i_dts );
        break;

    case SDT_TABLE_ID_ACTUAL:
        HandleSDTSection( i_pid, p_section, i_dts );
        break;

    default:
        if ( i_table_id == EIT_TABLE_ID_PF_ACTUAL ||
             (i_table_id >= EIT_TABLE_ID_SCHED_ACTUAL_FIRST &&
              i_table_id <= EIT_TABLE_ID_SCHED_ACTUAL_LAST) )
        {
            HandleEIT( i_pid, p_section, i_dts );
            break;
        }
        free( p_section );
        break;
    }
}

/*****************************************************************************
 * HandlePSIPacket
 *****************************************************************************/
static void HandlePSIPacket( uint8_t *p_ts, mtime_t i_dts )
{
    uint16_t i_pid = ts_get_pid( p_ts );
    ts_pid_t *p_pid = &p_pids[i_pid];
    uint8_t i_cc = ts_get_cc( p_ts );
    const uint8_t *p_payload;
    uint8_t i_length;

    if ( ts_check_duplicate( i_cc, p_pid->i_last_cc )
          || !ts_has_payload( p_ts ) )
        return;

    if ( p_pid->i_last_cc != -1
          && ts_check_discontinuity( i_cc, p_pid->i_last_cc ) )
        psi_assemble_reset( &p_pid->p_psi_buffer, &p_pid->i_psi_buffer_used );

    p_payload = ts_section( p_ts );
    i_length = p_ts + TS_SIZE - p_payload;

    if ( !psi_assemble_empty( &p_pid->p_psi_buffer,
                              &p_pid->i_psi_buffer_used ) )
    {
        uint8_t *p_section = psi_assemble_payload( &p_pid->p_psi_buffer,
                                                   &p_pid->i_psi_buffer_used,
                                                   &p_payload, &i_length );
        if ( p_section != NULL )
            HandleSection( i_pid, p_section, i_dts );
    }

    p_payload = ts_next_section( p_ts );
    i_length = p_ts + TS_SIZE - p_payload;

    while ( i_length )
    {
        uint8_t *p_section = psi_assemble_payload( &p_pid->p_psi_buffer,
                                                   &p_pid->i_psi_buffer_used,
                                                   &p_payload, &i_length );
        if ( p_section != NULL )
            HandleSection( i_pid, p_section, i_dts );
    }
}
