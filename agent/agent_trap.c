/* agent_trap.c: define trap generation routines for mib modules, etc,
   to use */

#include <config.h>

#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#if TIME_WITH_SYS_TIME
# ifdef WIN32
#  include <sys/timeb.h>
# else
#  include <sys/time.h>
# endif
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#if HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#elif HAVE_WINSOCK_H
#include <winsock.h>
#endif
#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#include "asn1.h"
#include "snmp_api.h"
#include "snmp_impl.h"
#include "snmp_client.h"
#include "snmp.h"
#include "system.h"
#include "read_config.h"
#include "snmp_debug.h"

struct trap_sink {
    struct snmp_session	*sesp;
    struct trap_sink	*next;
    int			pdutype;
    int			version;
};

struct trap_sink *sinks	  = NULL;

extern struct timeval	starttime;

#define OID_LENGTH(x)  (sizeof(x)/sizeof(x[0]))

oid objid_enterprisetrap[] = { EXTENSIBLEMIB, 251 };
oid version_id[]	   = { EXTENSIBLEMIB, AGENTID, OSTYPE };
int enterprisetrap_len = OID_LENGTH( objid_enterprisetrap );
int version_id_len     = OID_LENGTH( version_id );

#define SNMPV2_TRAPS_PREFIX	SNMP_OID_SNMPMODULES,1,1,5
oid  cold_start_oid[] =		{ SNMPV2_TRAPS_PREFIX, 1 };	/* SNMPv2-MIB */
oid  warm_start_oid[] =		{ SNMPV2_TRAPS_PREFIX, 2 };	/* SNMPv2-MIB */
oid  link_down_oid[] =		{ SNMPV2_TRAPS_PREFIX, 3 };	/* IF-MIB */
oid  link_up_oid[] =		{ SNMPV2_TRAPS_PREFIX, 4 };	/* IF-MIB */
oid  auth_fail_oid[] =		{ SNMPV2_TRAPS_PREFIX, 5 };	/* SNMPv2-MIB */
oid  egp_xxx_oid[] =		{ SNMPV2_TRAPS_PREFIX, 99 };	/* ??? */

#define SNMPV2_TRAP_OBJS_PREFIX	SNMP_OID_SNMPMODULES,1,1,4
oid  snmptrap_oid[] 	      =	{ SNMPV2_TRAP_OBJS_PREFIX, 1 };
oid  snmptrapenterprise_oid[] =	{ SNMPV2_TRAP_OBJS_PREFIX, 3 };
oid  sysuptime_oid[] 	      =	{ SNMP_OID_MIB2,1,3,0 };
int  snmptrap_oid_len 	      =	OID_LENGTH(snmptrap_oid);
int  snmptrapenterprise_oid_len = OID_LENGTH(snmptrapenterprise_oid);
int  sysuptime_oid_len 	      =	OID_LENGTH(sysuptime_oid);


#define SNMP_AUTHENTICATED_TRAPS_ENABLED	1
#define SNMP_AUTHENTICATED_TRAPS_DISABLED	2

int	 snmp_enableauthentraps	= SNMP_AUTHENTICATED_TRAPS_DISABLED;
char	*snmp_trapcommunity	= NULL;

/* Prototypes */
 /*
static int create_v1_trap_session (const char *, const char *);
static int create_v2_trap_session (const char *, const char *);
static int create_v2_inform_session (const char *, const char *);
static void free_trap_session (struct trap_sink *sp);
static void send_v1_trap (struct snmp_session *, int, int);
static void send_v2_trap (struct snmp_session *, int, int, int);
 */


	/*******************
	 *
	 * Trap session handling
	 *
	 *******************/
int add_trap_session( struct snmp_session *ss, int pdutype, int version )
{
    struct trap_sink *new_sink =
      (struct trap_sink *) malloc (sizeof (*new_sink));
    if ( new_sink == NULL )
	return 0;

    new_sink->sesp    = ss;
    new_sink->pdutype = pdutype;
    new_sink->version = version;
    new_sink->next    = sinks;
    sinks = new_sink;
    return 1;
}

int create_trap_session (char *sink, 
				char *com,
				int version, int pdutype)
{
    struct snmp_session	 session, *sesp;

    memset (&session, 0, sizeof (struct snmp_session));
    session.peername = sink;
    session.version = version;
    if (com) {
        session.community = (u_char *)com;
        session.community_len = strlen (com);
    }
    session.remote_port = SNMP_TRAP_PORT;
    sesp = snmp_open (&session);

    if (sesp) {
	return( add_trap_session( sesp, pdutype, version ));
    }

    /* diagnose snmp_open errors with the input struct snmp_session pointer */
    snmp_sess_perror("snmpd: create_trap_session", &session);
    return 0;
}

static int create_v1_trap_session (char *sink, 
				   char *com)
{
    return create_trap_session( sink, com, SNMP_VERSION_1, SNMP_MSG_TRAP );
}

static int create_v2_trap_session (char *sink, 
				   char *com)
{
    return create_trap_session( sink, com, SNMP_VERSION_2c, SNMP_MSG_TRAP2 );
}

static int create_v2_inform_session (char *sink, 
				     char *com)
{
    return create_trap_session( sink, com, SNMP_VERSION_2c, SNMP_MSG_INFORM );
}


static void free_trap_session (struct trap_sink *sp)
{
    snmp_close(sp->sesp);
    free (sp);
}


void snmpd_free_trapsinks (void)
{
    struct trap_sink *sp = sinks;
    while (sp) {
	sinks = sinks->next;
	free_trap_session(sp);
	sp = sinks;
    }
}

	/*******************
	 *
	 * Trap handling
	 *
	 *******************/

void send_enterprise_trap_vars (int trap, 
		     int specific,
		     oid *enterprise, int enterprise_length,
		     struct variable_list *vars)
{
    struct variable_list uptime_var, snmptrap_var, enterprise_var;
    struct variable_list *v2_vars, *last_var=NULL;
    struct snmp_pdu	*template_pdu, *pdu;
    struct timeval	 now;
    long uptime;
    struct sockaddr_in *pduIp;
    struct trap_sink *sink;
    
		/*
		 * Initialise SNMPv2 required variables
		 */
    gettimeofday(&now, NULL);
    uptime = calculate_time_diff(&now, &starttime);
    memset (&uptime_var, 0, sizeof (struct variable_list));
    snmp_set_var_objid( &uptime_var, sysuptime_oid, OID_LENGTH(sysuptime_oid));
    snmp_set_var_value( &uptime_var, (char *)&uptime, sizeof(uptime) );
    uptime_var.type           = ASN_TIMETICKS;
    uptime_var.next_variable  = &snmptrap_var;

    memset (&snmptrap_var, 0, sizeof (struct variable_list));
    snmp_set_var_objid( &snmptrap_var, snmptrap_oid, OID_LENGTH(snmptrap_oid));
	/* value set later .... */
    snmptrap_var.type           = ASN_OBJECT_ID;
    if ( vars )
	snmptrap_var.next_variable  = vars;
    else
	snmptrap_var.next_variable  = &enterprise_var;

			/* find end of provided varbind list,
			   ready to append the enterprise info if necessary */
    last_var = vars;
    while ( last_var && last_var->next_variable )
	last_var = last_var->next_variable;

    memset (&enterprise_var, 0, sizeof (struct variable_list));
    snmp_set_var_objid( &enterprise_var,
		 snmptrapenterprise_oid, OID_LENGTH(snmptrapenterprise_oid));
    snmp_set_var_value( &enterprise_var, (char *)enterprise, enterprise_length*sizeof(oid));
    enterprise_var.type           = ASN_OBJECT_ID;
    enterprise_var.next_variable  = NULL;

    v2_vars = &uptime_var;

		/*
		 *  Create a template PDU, ready for sending
		 */
    template_pdu = snmp_pdu_create( SNMP_MSG_TRAP );
    if ( template_pdu == NULL )
	return;
    template_pdu->trap_type     = trap;
    template_pdu->specific_type = specific;
    if ( snmp_clone_mem((void **)&template_pdu->enterprise,
				enterprise, enterprise_length*sizeof(oid))) {
	snmp_free_pdu( template_pdu );
	return;
    }
    template_pdu->enterprise_length = enterprise_length;
    template_pdu->flags |= UCD_MSG_FLAG_FORCE_PDU_COPY;
    pduIp = (struct sockaddr_in *)&template_pdu->agent_addr;
    pduIp->sin_family		 = AF_INET;
    pduIp->sin_addr.s_addr	 = get_myaddr();
    template_pdu->time		 	 = uptime;

		/*
		 *  Now use the parameters to determine
		 *    which v2 variables are needed,
		 *    and what values they should take.
		 */
    switch ( trap ) {
	case -1:	/*
			 *	SNMPv2 only
			 *  Check to see whether the variables provided
			 *    are sufficient for SNMPv2 notifications
			 */
		if (vars && snmp_oid_compare(vars->name, vars->name_length,
				sysuptime_oid, OID_LENGTH(sysuptime_oid)) == 0 )
			v2_vars = vars;
		else
		if (vars && snmp_oid_compare(vars->name, vars->name_length,
				snmptrap_oid, OID_LENGTH(snmptrap_oid)) == 0 )
			uptime_var.next_variable = vars;
		else {
			/* Hmmm... we don't seem to have a value - oops! */
			snmptrap_var.next_variable = vars;
		}
		last_var = NULL;	/* Don't need enterprise info */
		break;

			/* "Standard" SNMPv1 traps */

	case SNMP_TRAP_COLDSTART:
		snmp_set_var_value( &snmptrap_var,
				    (char *)cold_start_oid,
				    sizeof(cold_start_oid));
		break;
	case SNMP_TRAP_WARMSTART:
		snmp_set_var_value( &snmptrap_var,
				    (char *)warm_start_oid,
				    sizeof(warm_start_oid));
		break;
	case SNMP_TRAP_LINKDOWN:
		snmp_set_var_value( &snmptrap_var,
				    (char *)link_down_oid,
				    sizeof(link_down_oid));
		break;
	case SNMP_TRAP_LINKUP:
		snmp_set_var_value( &snmptrap_var,
				    (char *)link_up_oid,
				    sizeof(link_up_oid));
		break;
	case SNMP_TRAP_AUTHFAIL:
		if (snmp_enableauthentraps == SNMP_AUTHENTICATED_TRAPS_DISABLED)
		    return;
		snmp_set_var_value( &snmptrap_var,
				    (char *)auth_fail_oid,
				    sizeof(auth_fail_oid));
		break;
	case SNMP_TRAP_EGPNEIGHBORLOSS:
		snmp_set_var_value( &snmptrap_var,
				    (char *)egp_xxx_oid,
				    sizeof(egp_xxx_oid));
		break;

	case SNMP_TRAP_ENTERPRISESPECIFIC:
		snmp_set_var_value( &snmptrap_var,
				    (char *)enterprise,
				    (enterprise_length+2)*sizeof(oid));
		snmptrap_var.val.objid[ enterprise_length   ] = 0;
		snmptrap_var.val.objid[ enterprise_length+1 ] = specific;
		snmptrap_var.next_variable  = vars;
		last_var = NULL;	/* Don't need version info */
		break;
    }
    

		/*
		 *  Now loop through the list of trap sinks,
		 *   sending an appropriately formatted PDU to each
		 */
    for ( sink = sinks ; sink ; sink=sink->next ) {
	if ( sink->version == SNMP_VERSION_1 && trap == -1 )
		continue;	/* Skip v1 sinks for v2 only traps */
	template_pdu->version = sink->version;
	template_pdu->command = sink->pdutype;
	if ( sink->version != SNMP_VERSION_1 ) {
	    template_pdu->variables = v2_vars;
	    if ( last_var )
		last_var->next_variable = &enterprise_var;
	}
	else
	    template_pdu->variables = vars;

	pdu = snmp_clone_pdu( template_pdu );
	pdu->sessid = sink->sesp->sessid;	/* AgentX only ? */
	if ( snmp_send( sink->sesp, pdu) == 0 ) {
            snmp_sess_perror ("snmpd: send_trap", sink->sesp);
	    snmp_free_pdu( pdu );
	}
	else {
	    snmp_increment_statistic(STAT_SNMPOUTTRAPS);
	    snmp_increment_statistic(STAT_SNMPOUTPKTS);
	}
		
	if ( sink->version != SNMP_VERSION_1 && last_var )
	    last_var->next_variable = NULL;
    }

	/* Ensure we don't free anything we shouldn't */
    if ( last_var )
	last_var->next_variable = NULL;
    template_pdu->variables = NULL;
    snmp_free_pdu( template_pdu );
}

void send_trap_vars (int trap, 
		     int specific,
		     struct variable_list *vars)
{
    if ( trap == SNMP_TRAP_ENTERPRISESPECIFIC )
        send_enterprise_trap_vars( trap, specific, objid_enterprisetrap,
			OID_LENGTH(objid_enterprisetrap), vars );
    else
        send_enterprise_trap_vars( trap, specific, version_id,
			OID_LENGTH(version_id), vars );
}

void send_easy_trap (int trap, 
		     int specific)
{
    send_trap_vars( trap, specific, NULL );
}

void send_v2trap ( struct variable_list *vars)
{
    send_trap_vars( -1, -1, vars );
}

void
send_trap_pdu(struct snmp_pdu *pdu)
{
    send_trap_vars( -1, -1, pdu->variables );
}



	/*******************
	 *
	 * Config file handling
	 *
	 *******************/

void snmpd_parse_config_authtrap(const char *token, 
				 char *cptr)
{
    int i;

    i = atoi(cptr);
    if ( i == 0 ) {
	if ( !strcmp( cptr, "enable" ))
	    i = SNMP_AUTHENTICATED_TRAPS_ENABLED;
	else if ( !strcmp( cptr, "disable" ))
	    i = SNMP_AUTHENTICATED_TRAPS_DISABLED;
    }
    if (i < 1 || i > 2)
	config_perror("authtrapenable must be 1 or 2");
    else
	snmp_enableauthentraps = i;
}

void snmpd_parse_config_trapsink(const char *token, 
				 char *cptr)
{
    char tmpbuf[1024];
    char *sp, *cp;
  
    if (!snmp_trapcommunity) snmp_trapcommunity = strdup("public");
    sp = strtok(cptr, " \t\n");
    cp = strtok(NULL, " \t\n");
    if (create_v1_trap_session(sp, cp ? cp : snmp_trapcommunity) == 0) {
	sprintf(tmpbuf,"cannot create trapsink: %s", cptr);
	config_perror(tmpbuf);
    }
}


void
snmpd_parse_config_trap2sink(const char *word, char *cptr)
{
    char tmpbuf[1024];
    char *sp, *cp;
  
    if (!snmp_trapcommunity) snmp_trapcommunity = strdup("public");
    sp = strtok(cptr, " \t\n");
    cp = strtok(NULL, " \t\n");
    if (create_v2_trap_session(sp, cp ? cp : snmp_trapcommunity) == 0) {
	sprintf(tmpbuf,"cannot create trap2sink: %s", cptr);
	config_perror(tmpbuf);
    }
}

void
snmpd_parse_config_informsink(const char *word, char *cptr)
{
    char tmpbuf[1024];
    char *sp, *cp;
  
    if (!snmp_trapcommunity) snmp_trapcommunity = strdup("public");
    sp = strtok(cptr, " \t\n");
    cp = strtok(NULL, " \t\n");
    if (create_v2_inform_session(sp, cp ? cp : snmp_trapcommunity) == 0) {
	sprintf(tmpbuf,"cannot create informsink: %s", cptr);
	config_perror(tmpbuf);
    }
}

void
snmpd_parse_config_trapcommunity(const char *word, char *cptr)
{
    if (snmp_trapcommunity) free(snmp_trapcommunity);
    snmp_trapcommunity = malloc (strlen(cptr)+1);
    copy_word(cptr, snmp_trapcommunity);
}

void snmpd_free_trapcommunity (void)
{
    if (snmp_trapcommunity) {
	free(snmp_trapcommunity);
	snmp_trapcommunity = NULL;
    }
}
