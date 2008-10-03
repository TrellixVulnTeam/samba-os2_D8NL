/* 
   Samba Unix/Linux SMB client library 
   Distributed SMB/CIFS Server Management Utility 
   Copyright (C) 2001 Andrew Bartlett (abartlet@samba.org)
   Copyright (C) 2002 Jim McDonough (jmcd@us.ibm.com)
   Copyright (C) 2004,2008 Guenther Deschner (gd@samba.org)
   Copyright (C) 2005 Jeremy Allison (jra@samba.org)
   Copyright (C) 2006 Jelmer Vernooij (jelmer@samba.org)

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */
 
#include "includes.h"
#include "utils/net.h"

static int net_mode_share;
static bool sync_files(struct copy_clistate *cp_clistate, const char *mask);

/**
 * @file net_rpc.c
 *
 * @brief RPC based subcommands for the 'net' utility.
 *
 * This file should contain much of the functionality that used to
 * be found in rpcclient, execpt that the commands should change 
 * less often, and the fucntionality should be sane (the user is not 
 * expected to know a rid/sid before they conduct an operation etc.)
 *
 * @todo Perhaps eventually these should be split out into a number
 * of files, as this could get quite big.
 **/


/**
 * Many of the RPC functions need the domain sid.  This function gets
 *  it at the start of every run 
 *
 * @param cli A cli_state already connected to the remote machine
 *
 * @return The Domain SID of the remote machine.
 **/

NTSTATUS net_get_remote_domain_sid(struct cli_state *cli, TALLOC_CTX *mem_ctx,
				   DOM_SID **domain_sid,
				   const char **domain_name)
{
	struct rpc_pipe_client *lsa_pipe;
	POLICY_HND pol;
	NTSTATUS result = NT_STATUS_OK;
	union lsa_PolicyInformation *info = NULL;

	lsa_pipe = cli_rpc_pipe_open_noauth(cli, PI_LSARPC, &result);
	if (!lsa_pipe) {
		d_fprintf(stderr, "Could not initialise lsa pipe\n");
		return result;
	}
	
	result = rpccli_lsa_open_policy(lsa_pipe, mem_ctx, False, 
				     SEC_RIGHTS_MAXIMUM_ALLOWED,
				     &pol);
	if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "open_policy failed: %s\n",
			  nt_errstr(result));
		return result;
	}

	result = rpccli_lsa_QueryInfoPolicy(lsa_pipe, mem_ctx,
					    &pol,
					    LSA_POLICY_INFO_ACCOUNT_DOMAIN,
					    &info);
	if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "lsaquery failed: %s\n",
			  nt_errstr(result));
		return result;
	}

	*domain_name = info->account_domain.name.string;
	*domain_sid = info->account_domain.sid;

	rpccli_lsa_Close(lsa_pipe, mem_ctx, &pol);
	cli_rpc_pipe_close(lsa_pipe);

	return NT_STATUS_OK;
}

/**
 * Run a single RPC command, from start to finish.
 *
 * @param pipe_name the pipe to connect to (usually a PIPE_ constant)
 * @param conn_flag a NET_FLAG_ combination.  Passed to 
 *                   net_make_ipc_connection.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 * @return A shell status integer (0 for success).
 */

int run_rpc_command(struct cli_state *cli_arg,
			const int pipe_idx,
			int conn_flags,
			rpc_command_fn fn,
			int argc,
			const char **argv) 
{
	struct cli_state *cli = NULL;
	struct rpc_pipe_client *pipe_hnd = NULL;
	TALLOC_CTX *mem_ctx;
	NTSTATUS nt_status;
	DOM_SID *domain_sid;
	const char *domain_name;

	/* make use of cli_state handed over as an argument, if possible */
	if (!cli_arg) {
		nt_status = net_make_ipc_connection(conn_flags, &cli);
		if (!NT_STATUS_IS_OK(nt_status)) {
			DEBUG(1, ("failed to make ipc connection: %s\n",
				  nt_errstr(nt_status)));
			return -1;
		}
	} else {
		cli = cli_arg;
	}

	if (!cli) {
		return -1;
	}

	/* Create mem_ctx */
	
	if (!(mem_ctx = talloc_init("run_rpc_command"))) {
		DEBUG(0, ("talloc_init() failed\n"));
		cli_shutdown(cli);
		return -1;
	}
	
	nt_status = net_get_remote_domain_sid(cli, mem_ctx, &domain_sid,
					      &domain_name);
	if (!NT_STATUS_IS_OK(nt_status)) {
		cli_shutdown(cli);
		return -1;
	}

	if (!(conn_flags & NET_FLAGS_NO_PIPE)) {
		if (lp_client_schannel() && (pipe_idx == PI_NETLOGON)) {
			/* Always try and create an schannel netlogon pipe. */
			pipe_hnd = cli_rpc_pipe_open_schannel(cli, pipe_idx,
							PIPE_AUTH_LEVEL_PRIVACY,
							domain_name,
							&nt_status);
			if (!pipe_hnd) {
				DEBUG(0, ("Could not initialise schannel netlogon pipe. Error was %s\n",
					nt_errstr(nt_status) ));
				cli_shutdown(cli);
				return -1;
			}
		} else {
			pipe_hnd = cli_rpc_pipe_open_noauth(cli, pipe_idx, &nt_status);
			if (!pipe_hnd) {
				DEBUG(0, ("Could not initialise pipe %s. Error was %s\n",
					cli_get_pipe_name(pipe_idx),
					nt_errstr(nt_status) ));
				cli_shutdown(cli);
				return -1;
			}
		}
	}
	
	nt_status = fn(domain_sid, domain_name, cli, pipe_hnd, mem_ctx, argc, argv);
	
	if (!NT_STATUS_IS_OK(nt_status)) {
		DEBUG(1, ("rpc command function failed! (%s)\n", nt_errstr(nt_status)));
	} else {
		DEBUG(5, ("rpc command function succedded\n"));
	}
		
	if (!(conn_flags & NET_FLAGS_NO_PIPE)) {
		if (pipe_hnd) {
			cli_rpc_pipe_close(pipe_hnd);
		}
	}

	/* close the connection only if it was opened here */
	if (!cli_arg) {
		cli_shutdown(cli);
	}
	
	talloc_destroy(mem_ctx);
	return (!NT_STATUS_IS_OK(nt_status));
}

/** 
 * Force a change of the trust acccount password.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_changetrustpw_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx, 
					int argc,
					const char **argv)
{
	
	return trust_pw_find_change_and_store_it(pipe_hnd, mem_ctx, opt_target_workgroup);
}

/** 
 * Force a change of the trust acccount password.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/

int net_rpc_changetrustpw(int argc, const char **argv) 
{
	return run_rpc_command(NULL, PI_NETLOGON, NET_FLAGS_ANONYMOUS | NET_FLAGS_PDC, 
			       rpc_changetrustpw_internals,
			       argc, argv);
}

/** 
 * Join a domain, the old way.
 *
 * This uses 'machinename' as the inital password, and changes it. 
 *
 * The password should be created with 'server manager' or equiv first.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_oldjoin_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli, 
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx, 
					int argc,
					const char **argv)
{
	
	fstring trust_passwd;
	unsigned char orig_trust_passwd_hash[16];
	NTSTATUS result;
	uint32 sec_channel_type;

	pipe_hnd = cli_rpc_pipe_open_noauth(cli, PI_NETLOGON, &result);
	if (!pipe_hnd) {
		DEBUG(0,("rpc_oldjoin_internals: netlogon pipe open to machine %s failed. "
			"error was %s\n",
			cli->desthost,
			nt_errstr(result) ));
		return result;
	}

	/* 
	   check what type of join - if the user want's to join as
	   a BDC, the server must agree that we are a BDC.
	*/
	if (argc >= 0) {
		sec_channel_type = get_sec_channel_type(argv[0]);
	} else {
		sec_channel_type = get_sec_channel_type(NULL);
	}
	
	fstrcpy(trust_passwd, global_myname());
	strlower_m(trust_passwd);

	/*
	 * Machine names can be 15 characters, but the max length on
	 * a password is 14.  --jerry
	 */

	trust_passwd[14] = '\0';

	E_md4hash(trust_passwd, orig_trust_passwd_hash);

	result = trust_pw_change_and_store_it(pipe_hnd, mem_ctx, opt_target_workgroup,
					      orig_trust_passwd_hash,
					      sec_channel_type);

	if (NT_STATUS_IS_OK(result))
		printf("Joined domain %s.\n",opt_target_workgroup);


	if (!secrets_store_domain_sid(opt_target_workgroup, domain_sid)) {
		DEBUG(0, ("error storing domain sid for %s\n", opt_target_workgroup));
		result = NT_STATUS_UNSUCCESSFUL;
	}

	return result;
}

/** 
 * Join a domain, the old way.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success)
 **/

static int net_rpc_perform_oldjoin(int argc, const char **argv)
{
	return run_rpc_command(NULL, PI_NETLOGON, 
			       NET_FLAGS_NO_PIPE | NET_FLAGS_ANONYMOUS | NET_FLAGS_PDC, 
			       rpc_oldjoin_internals,
			       argc, argv);
}

/** 
 * Join a domain, the old way.  This function exists to allow
 * the message to be displayed when oldjoin was explicitly 
 * requested, but not when it was implied by "net rpc join".
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success)
 **/

static int net_rpc_oldjoin(int argc, const char **argv) 
{
	int rc = net_rpc_perform_oldjoin(argc, argv);

	if (rc) {
		d_fprintf(stderr, "Failed to join domain\n");
	}

	return rc;
}

/** 
 * Basic usage function for 'net rpc join'.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/

static int rpc_join_usage(int argc, const char **argv) 
{	
	d_printf("net rpc join -U <username>[%%password] <type>[options]\n"\
		 "\t to join a domain with admin username & password\n"\
		 "\t\t password will be prompted if needed and none is specified\n"\
		 "\t <type> can be (default MEMBER)\n"\
		 "\t\t BDC - Join as a BDC\n"\
		 "\t\t PDC - Join as a PDC\n"\
		 "\t\t MEMBER - Join as a MEMBER server\n");

	net_common_flags_usage(argc, argv);
	return -1;
}

/** 
 * 'net rpc join' entrypoint.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * Main 'net_rpc_join()' (where the admin username/password is used) is 
 * in net_rpc_join.c.
 * Try to just change the password, but if that doesn't work, use/prompt
 * for a username/password.
 **/

int net_rpc_join(int argc, const char **argv) 
{
	if (lp_server_role() == ROLE_STANDALONE) {
		d_printf("cannot join as standalone machine\n");
		return -1;
	}

	if (strlen(global_myname()) > 15) {
		d_printf("Our netbios name can be at most 15 chars long, "
			 "\"%s\" is %u chars long\n",
			 global_myname(), (unsigned int)strlen(global_myname()));
		return -1;
	}

	if ((net_rpc_perform_oldjoin(argc, argv) == 0))
		return 0;
	
	return net_rpc_join_newstyle(argc, argv);
}

/** 
 * display info about a rpc domain
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

NTSTATUS rpc_info_internals(const DOM_SID *domain_sid,
			const char *domain_name, 
			struct cli_state *cli,
			struct rpc_pipe_client *pipe_hnd,
			TALLOC_CTX *mem_ctx,
			int argc,
			const char **argv)
{
	POLICY_HND connect_pol, domain_pol;
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	union samr_DomainInfo *info = NULL;
	fstring sid_str;

	sid_to_fstring(sid_str, domain_sid);

	/* Get sam policy handle */
	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);
	if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "Could not connect to SAM: %s\n", nt_errstr(result));
		goto done;
	}

	/* Get domain policy handle */
	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "Could not open domain: %s\n", nt_errstr(result));
		goto done;
	}

	result = rpccli_samr_QueryDomainInfo(pipe_hnd, mem_ctx,
					     &domain_pol,
					     2,
					     &info);
	if (NT_STATUS_IS_OK(result)) {
		d_printf("Domain Name: %s\n", info->info2.domain_name.string);
		d_printf("Domain SID: %s\n", sid_str);
		d_printf("Sequence number: %llu\n",
			(unsigned long long)info->info2.sequence_num);
		d_printf("Num users: %u\n", info->info2.num_users);
		d_printf("Num domain groups: %u\n", info->info2.num_groups);
		d_printf("Num local groups: %u\n", info->info2.num_aliases);
	}

 done:
	return result;
}

/** 
 * 'net rpc info' entrypoint.
 * @param argc  Standard main() style argc
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/

int net_rpc_info(int argc, const char **argv) 
{
	return run_rpc_command(NULL, PI_SAMR, NET_FLAGS_PDC, 
			       rpc_info_internals,
			       argc, argv);
}

/** 
 * Fetch domain SID into the local secrets.tdb
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_getsid_internals(const DOM_SID *domain_sid,
			const char *domain_name, 
			struct cli_state *cli,
			struct rpc_pipe_client *pipe_hnd,
			TALLOC_CTX *mem_ctx,
			int argc,
			const char **argv)
{
	fstring sid_str;

	sid_to_fstring(sid_str, domain_sid);
	d_printf("Storing SID %s for Domain %s in secrets.tdb\n",
		 sid_str, domain_name);

	if (!secrets_store_domain_sid(domain_name, domain_sid)) {
		DEBUG(0,("Can't store domain SID\n"));
		return NT_STATUS_UNSUCCESSFUL;
	}

	return NT_STATUS_OK;
}

/** 
 * 'net rpc getsid' entrypoint.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/

int net_rpc_getsid(int argc, const char **argv) 
{
	return run_rpc_command(NULL, PI_SAMR, NET_FLAGS_ANONYMOUS | NET_FLAGS_PDC, 
			       rpc_getsid_internals,
			       argc, argv);
}

/****************************************************************************/

/**
 * Basic usage function for 'net rpc user'.
 * @param argc	Standard main() style argc.
 * @param argv	Standard main() style argv. Initial components are already
 *		stripped.
 **/

static int rpc_user_usage(int argc, const char **argv)
{
	return net_help_user(argc, argv);
}

/** 
 * Add a new user to a remote RPC server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv.  Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success)
 **/

static int rpc_user_add(int argc, const char **argv) 
{
	NET_API_STATUS status;
	struct USER_INFO_1 info1;
	uint32_t parm_error = 0;

	if (argc < 1) {
		d_printf("User must be specified\n");
		rpc_user_usage(argc, argv);
		return 0;
	}

	ZERO_STRUCT(info1);

	info1.usri1_name = argv[0];
	if (argc == 2) {
		info1.usri1_password = argv[1];
	}

	status = NetUserAdd(opt_host, 1, (uint8_t *)&info1, &parm_error);

	if (status != 0) {
		d_fprintf(stderr, "Failed to add user '%s' with: %s.\n",
			argv[0], libnetapi_get_error_string(netapi_ctx, status));
		return -1;
	} else {
		d_printf("Added user '%s'.\n", argv[0]);
	}

	return 0;
}

/** 
 * Rename a user on a remote RPC server.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_user_rename_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx, 
					int argc,
					const char **argv)
{
	POLICY_HND connect_pol, domain_pol, user_pol;
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	uint32 info_level = 7;
	const char *old_name, *new_name;
	struct samr_Ids user_rids, name_types;
	struct lsa_String lsa_acct_name;
	union samr_UserInfo *info = NULL;

	if (argc != 2) {
		d_printf("Old and new username must be specified\n");
		rpc_user_usage(argc, argv);
		return NT_STATUS_OK;
	}

	old_name = argv[0];
	new_name = argv[1];

	/* Get sam policy handle */

	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);

	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}
	
	/* Get domain policy handle */

	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	init_lsa_String(&lsa_acct_name, old_name);

	result = rpccli_samr_LookupNames(pipe_hnd, mem_ctx,
					 &domain_pol,
					 1,
					 &lsa_acct_name,
					 &user_rids,
					 &name_types);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	/* Open domain user */
	result = rpccli_samr_OpenUser(pipe_hnd, mem_ctx,
				      &domain_pol,
				      MAXIMUM_ALLOWED_ACCESS,
				      user_rids.ids[0],
				      &user_pol);

	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	/* Query user info */
	result = rpccli_samr_QueryUserInfo(pipe_hnd, mem_ctx,
					   &user_pol,
					   info_level,
					   &info);

	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	init_samr_user_info7(&info->info7, new_name);

	/* Set new name */
	result = rpccli_samr_SetUserInfo2(pipe_hnd, mem_ctx,
					  &user_pol,
					  info_level,
					  info);

	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

 done:
	if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "Failed to rename user from %s to %s - %s\n", old_name, new_name, 
			 nt_errstr(result));
	} else {
		d_printf("Renamed user from %s to %s\n", old_name, new_name);
	}
	return result;
}

/** 
 * Rename a user on a remote RPC server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/

static int rpc_user_rename(int argc, const char **argv) 
{
	return run_rpc_command(NULL, PI_SAMR, 0, rpc_user_rename_internals,
			       argc, argv);
}

/** 
 * Delete a user from a remote RPC server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/

static int rpc_user_delete(int argc, const char **argv) 
{
	NET_API_STATUS status;

	if (argc < 1) {
		d_printf("User must be specified\n");
		rpc_user_usage(argc, argv);
		return 0;
	}

	status = NetUserDel(opt_host, argv[0]);

	if (status != 0) {
                d_fprintf(stderr, "Failed to delete user '%s' with: %s.\n",
			  argv[0],
			  libnetapi_get_error_string(netapi_ctx, status));
		return -1;
        } else {
                d_printf("Deleted user '%s'.\n", argv[0]);
        }

	return 0;
}

/** 
 * Set a password for a user on a remote RPC server.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_user_password_internals(const DOM_SID *domain_sid, 
					const char *domain_name, 
					struct cli_state *cli, 
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx, 
					int argc,
					const char **argv)
{
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	POLICY_HND connect_pol, domain_pol, user_pol;
	uchar pwbuf[516];
	const char *user;
	const char *new_password;
	char *prompt = NULL;
	union samr_UserInfo info;

	if (argc < 1) {
		d_printf("User must be specified\n");
		rpc_user_usage(argc, argv);
		return NT_STATUS_OK;
	}
	
	user = argv[0];

	if (argv[1]) {
		new_password = argv[1];
	} else {
		asprintf(&prompt, "Enter new password for %s:", user);
		new_password = getpass(prompt);
		SAFE_FREE(prompt);
	}

	/* Get sam policy and domain handles */

	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);

	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);

	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	/* Get handle on user */

	{
		struct samr_Ids user_rids, name_types;
		struct lsa_String lsa_acct_name;

		init_lsa_String(&lsa_acct_name, user);

		result = rpccli_samr_LookupNames(pipe_hnd, mem_ctx,
						 &domain_pol,
						 1,
						 &lsa_acct_name,
						 &user_rids,
						 &name_types);
		if (!NT_STATUS_IS_OK(result)) {
			goto done;
		}

		result = rpccli_samr_OpenUser(pipe_hnd, mem_ctx,
					      &domain_pol,
					      MAXIMUM_ALLOWED_ACCESS,
					      user_rids.ids[0],
					      &user_pol);

		if (!NT_STATUS_IS_OK(result)) {
			goto done;
		}
	}

	/* Set password on account */

	encode_pw_buffer(pwbuf, new_password, STR_UNICODE);

	init_samr_user_info24(&info.info24, pwbuf, 24);

	SamOEMhashBlob(info.info24.password.data, 516,
		       &cli->user_session_key);

	result = rpccli_samr_SetUserInfo2(pipe_hnd, mem_ctx,
					  &user_pol,
					  24,
					  &info);

	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	/* Display results */

 done:
	return result;

}	

/** 
 * Set a user's password on a remote RPC server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/

static int rpc_user_password(int argc, const char **argv) 
{
	return run_rpc_command(NULL, PI_SAMR, 0, rpc_user_password_internals,
			       argc, argv);
}

/** 
 * List user's groups on a remote RPC server.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv.  Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_user_info_internals(const DOM_SID *domain_sid,
			const char *domain_name, 
			struct cli_state *cli,
			struct rpc_pipe_client *pipe_hnd,
			TALLOC_CTX *mem_ctx,
			int argc,
			const char **argv)
{
	POLICY_HND connect_pol, domain_pol, user_pol;
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	int i;
	struct samr_RidWithAttributeArray *rid_array = NULL;
	struct lsa_Strings names;
	struct samr_Ids types;
	uint32_t *lrids = NULL;
	struct samr_Ids rids, name_types;
	struct lsa_String lsa_acct_name;


	if (argc < 1) {
		d_printf("User must be specified\n");
		rpc_user_usage(argc, argv);
		return NT_STATUS_OK;
	}
	/* Get sam policy handle */

	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);
	if (!NT_STATUS_IS_OK(result)) goto done;
	
	/* Get domain policy handle */

	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) goto done;

	/* Get handle on user */

	init_lsa_String(&lsa_acct_name, argv[0]);

	result = rpccli_samr_LookupNames(pipe_hnd, mem_ctx,
					 &domain_pol,
					 1,
					 &lsa_acct_name,
					 &rids,
					 &name_types);

	if (!NT_STATUS_IS_OK(result)) goto done;

	result = rpccli_samr_OpenUser(pipe_hnd, mem_ctx,
				      &domain_pol,
				      MAXIMUM_ALLOWED_ACCESS,
				      rids.ids[0],
				      &user_pol);
	if (!NT_STATUS_IS_OK(result)) goto done;

	result = rpccli_samr_GetGroupsForUser(pipe_hnd, mem_ctx,
					      &user_pol,
					      &rid_array);

	if (!NT_STATUS_IS_OK(result)) goto done;

	/* Look up rids */

	if (rid_array->count) {
		if ((lrids = TALLOC_ARRAY(mem_ctx, uint32, rid_array->count)) == NULL) {
			result = NT_STATUS_NO_MEMORY;
			goto done;
		}

		for (i = 0; i < rid_array->count; i++)
			lrids[i] = rid_array->rids[i].rid;

		result = rpccli_samr_LookupRids(pipe_hnd, mem_ctx,
						&domain_pol,
						rid_array->count,
						lrids,
						&names,
						&types);

		if (!NT_STATUS_IS_OK(result)) {
			goto done;
		}

		/* Display results */

		for (i = 0; i < names.count; i++)
			printf("%s\n", names.names[i].string);
	}
 done:
	return result;
}

/** 
 * List a user's groups from a remote RPC server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/

static int rpc_user_info(int argc, const char **argv) 
{
	return run_rpc_command(NULL, PI_SAMR, 0, rpc_user_info_internals,
			       argc, argv);
}

/** 
 * List users on a remote RPC server.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_user_list_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	POLICY_HND connect_pol, domain_pol;
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	uint32 start_idx=0, num_entries, i, loop_count = 0;

	/* Get sam policy handle */

	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}
	
	/* Get domain policy handle */

	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	/* Query domain users */
	if (opt_long_list_entries)
		d_printf("\nUser name             Comment"\
			 "\n-----------------------------\n");
	do {
		const char *user = NULL;
		const char *desc = NULL;
		uint32 max_entries, max_size;
		uint32_t total_size, returned_size;
		union samr_DispInfo info;

		get_query_dispinfo_params(
			loop_count, &max_entries, &max_size);

		result = rpccli_samr_QueryDisplayInfo(pipe_hnd, mem_ctx,
						      &domain_pol,
						      1,
						      start_idx,
						      max_entries,
						      max_size,
						      &total_size,
						      &returned_size,
						      &info);
		loop_count++;
		start_idx += info.info1.count;
		num_entries = info.info1.count;

		for (i = 0; i < num_entries; i++) {
			user = info.info1.entries[i].account_name.string;
			if (opt_long_list_entries)
				desc = info.info1.entries[i].description.string;
			if (opt_long_list_entries)
				printf("%-21.21s %s\n", user, desc);
			else
				printf("%s\n", user);
		}
	} while (NT_STATUS_EQUAL(result, STATUS_MORE_ENTRIES));

 done:
	return result;
}

/** 
 * 'net rpc user' entrypoint.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/

int net_rpc_user(int argc, const char **argv) 
{
	NET_API_STATUS status;

	struct functable func[] = {
		{"add", rpc_user_add},
		{"info", rpc_user_info},
		{"delete", rpc_user_delete},
		{"password", rpc_user_password},
		{"rename", rpc_user_rename},
		{NULL, NULL}
	};

	status = libnetapi_init(&netapi_ctx);
	if (status != 0) {
		return -1;
	}
	libnetapi_set_username(netapi_ctx, opt_user_name);
	libnetapi_set_password(netapi_ctx, opt_password);

	if (argc == 0) {
		return run_rpc_command(NULL,PI_SAMR, 0, 
				       rpc_user_list_internals,
				       argc, argv);
	}

	return net_run_function(argc, argv, func, rpc_user_usage);
}

static NTSTATUS rpc_sh_user_list(TALLOC_CTX *mem_ctx,
				 struct rpc_sh_ctx *ctx,
				 struct rpc_pipe_client *pipe_hnd,
				 int argc, const char **argv)
{
	return rpc_user_list_internals(ctx->domain_sid, ctx->domain_name,
				       ctx->cli, pipe_hnd, mem_ctx,
				       argc, argv);
}

static NTSTATUS rpc_sh_user_info(TALLOC_CTX *mem_ctx,
				 struct rpc_sh_ctx *ctx,
				 struct rpc_pipe_client *pipe_hnd,
				 int argc, const char **argv)
{
	return rpc_user_info_internals(ctx->domain_sid, ctx->domain_name,
				       ctx->cli, pipe_hnd, mem_ctx,
				       argc, argv);
}

static NTSTATUS rpc_sh_handle_user(TALLOC_CTX *mem_ctx,
				   struct rpc_sh_ctx *ctx,
				   struct rpc_pipe_client *pipe_hnd,
				   int argc, const char **argv,
				   NTSTATUS (*fn)(
					   TALLOC_CTX *mem_ctx,
					   struct rpc_sh_ctx *ctx,
					   struct rpc_pipe_client *pipe_hnd,
					   POLICY_HND *user_hnd,
					   int argc, const char **argv))
{
	POLICY_HND connect_pol, domain_pol, user_pol;
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	DOM_SID sid;
	uint32 rid;
	enum lsa_SidType type;

	if (argc == 0) {
		d_fprintf(stderr, "usage: %s <username>\n", ctx->whoami);
		return NT_STATUS_INVALID_PARAMETER;
	}

	ZERO_STRUCT(connect_pol);
	ZERO_STRUCT(domain_pol);
	ZERO_STRUCT(user_pol);

	result = net_rpc_lookup_name(mem_ctx, pipe_hnd->cli, argv[0],
				     NULL, NULL, &sid, &type);
	if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "Could not lookup %s: %s\n", argv[0],
			  nt_errstr(result));
		goto done;
	}

	if (type != SID_NAME_USER) {
		d_fprintf(stderr, "%s is a %s, not a user\n", argv[0],
			  sid_type_lookup(type));
		result = NT_STATUS_NO_SUCH_USER;
		goto done;
	}

	if (!sid_peek_check_rid(ctx->domain_sid, &sid, &rid)) {
		d_fprintf(stderr, "%s is not in our domain\n", argv[0]);
		result = NT_STATUS_NO_SUCH_USER;
		goto done;
	}

	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					ctx->domain_sid,
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	result = rpccli_samr_OpenUser(pipe_hnd, mem_ctx,
				      &domain_pol,
				      MAXIMUM_ALLOWED_ACCESS,
				      rid,
				      &user_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	result = fn(mem_ctx, ctx, pipe_hnd, &user_pol, argc-1, argv+1);

 done:
	if (is_valid_policy_hnd(&user_pol)) {
		rpccli_samr_Close(pipe_hnd, mem_ctx, &user_pol);
	}
	if (is_valid_policy_hnd(&domain_pol)) {
		rpccli_samr_Close(pipe_hnd, mem_ctx, &domain_pol);
	}
	if (is_valid_policy_hnd(&connect_pol)) {
		rpccli_samr_Close(pipe_hnd, mem_ctx, &connect_pol);
	}
	return result;
}

static NTSTATUS rpc_sh_user_show_internals(TALLOC_CTX *mem_ctx,
					   struct rpc_sh_ctx *ctx,
					   struct rpc_pipe_client *pipe_hnd,
					   POLICY_HND *user_hnd,
					   int argc, const char **argv)
{
	NTSTATUS result;
	union samr_UserInfo *info = NULL;

	if (argc != 0) {
		d_fprintf(stderr, "usage: %s show <username>\n", ctx->whoami);
		return NT_STATUS_INVALID_PARAMETER;
	}

	result = rpccli_samr_QueryUserInfo(pipe_hnd, mem_ctx,
					   user_hnd,
					   21,
					   &info);
	if (!NT_STATUS_IS_OK(result)) {
		return result;
	}

	d_printf("user rid: %d, group rid: %d\n",
		info->info21.rid,
		info->info21.primary_gid);

	return result;
}

static NTSTATUS rpc_sh_user_show(TALLOC_CTX *mem_ctx,
				 struct rpc_sh_ctx *ctx,
				 struct rpc_pipe_client *pipe_hnd,
				 int argc, const char **argv)
{
	return rpc_sh_handle_user(mem_ctx, ctx, pipe_hnd, argc, argv,
				  rpc_sh_user_show_internals);
}

#define FETCHSTR(name, rec) \
do { if (strequal(ctx->thiscmd, name)) { \
	oldval = talloc_strdup(mem_ctx, info->info21.rec.string); } \
} while (0);

#define SETSTR(name, rec, flag) \
do { if (strequal(ctx->thiscmd, name)) { \
	init_lsa_String(&(info->info21.rec), argv[0]); \
	info->info21.fields_present |= SAMR_FIELD_##flag; } \
} while (0);

static NTSTATUS rpc_sh_user_str_edit_internals(TALLOC_CTX *mem_ctx,
					       struct rpc_sh_ctx *ctx,
					       struct rpc_pipe_client *pipe_hnd,
					       POLICY_HND *user_hnd,
					       int argc, const char **argv)
{
	NTSTATUS result;
	const char *username;
	const char *oldval = "";
	union samr_UserInfo *info = NULL;

	if (argc > 1) {
		d_fprintf(stderr, "usage: %s <username> [new value|NULL]\n",
			  ctx->whoami);
		return NT_STATUS_INVALID_PARAMETER;
	}

	result = rpccli_samr_QueryUserInfo(pipe_hnd, mem_ctx,
					   user_hnd,
					   21,
					   &info);
	if (!NT_STATUS_IS_OK(result)) {
		return result;
	}

	username = talloc_strdup(mem_ctx, info->info21.account_name.string);

	FETCHSTR("fullname", full_name);
	FETCHSTR("homedir", home_directory);
	FETCHSTR("homedrive", home_drive);
	FETCHSTR("logonscript", logon_script);
	FETCHSTR("profilepath", profile_path);
	FETCHSTR("description", description);

	if (argc == 0) {
		d_printf("%s's %s: [%s]\n", username, ctx->thiscmd, oldval);
		goto done;
	}

	if (strcmp(argv[0], "NULL") == 0) {
		argv[0] = "";
	}

	ZERO_STRUCT(info->info21);

	SETSTR("fullname", full_name, FULL_NAME);
	SETSTR("homedir", home_directory, HOME_DIRECTORY);
	SETSTR("homedrive", home_drive, HOME_DRIVE);
	SETSTR("logonscript", logon_script, LOGON_SCRIPT);
	SETSTR("profilepath", profile_path, PROFILE_PATH);
	SETSTR("description", description, DESCRIPTION);

	result = rpccli_samr_SetUserInfo(pipe_hnd, mem_ctx,
					 user_hnd,
					 21,
					 info);

	d_printf("Set %s's %s from [%s] to [%s]\n", username,
		 ctx->thiscmd, oldval, argv[0]);

 done:

	return result;
}

#define HANDLEFLG(name, rec) \
do { if (strequal(ctx->thiscmd, name)) { \
	oldval = (oldflags & ACB_##rec) ? "yes" : "no"; \
	if (newval) { \
		newflags = oldflags | ACB_##rec; \
	} else { \
		newflags = oldflags & ~ACB_##rec; \
	} } } while (0);

static NTSTATUS rpc_sh_user_str_edit(TALLOC_CTX *mem_ctx,
				     struct rpc_sh_ctx *ctx,
				     struct rpc_pipe_client *pipe_hnd,
				     int argc, const char **argv)
{
	return rpc_sh_handle_user(mem_ctx, ctx, pipe_hnd, argc, argv,
				  rpc_sh_user_str_edit_internals);
}

static NTSTATUS rpc_sh_user_flag_edit_internals(TALLOC_CTX *mem_ctx,
						struct rpc_sh_ctx *ctx,
						struct rpc_pipe_client *pipe_hnd,
						POLICY_HND *user_hnd,
						int argc, const char **argv)
{
	NTSTATUS result;
	const char *username;
	const char *oldval = "unknown";
	uint32 oldflags, newflags;
	bool newval;
	union samr_UserInfo *info = NULL;

	if ((argc > 1) ||
	    ((argc == 1) && !strequal(argv[0], "yes") &&
	     !strequal(argv[0], "no"))) {
		d_fprintf(stderr, "usage: %s <username> [yes|no]\n",
			  ctx->whoami);
		return NT_STATUS_INVALID_PARAMETER;
	}

	newval = strequal(argv[0], "yes");

	result = rpccli_samr_QueryUserInfo(pipe_hnd, mem_ctx,
					   user_hnd,
					   21,
					   &info);
	if (!NT_STATUS_IS_OK(result)) {
		return result;
	}

	username = talloc_strdup(mem_ctx, info->info21.account_name.string);
	oldflags = info->info21.acct_flags;
	newflags = info->info21.acct_flags;

	HANDLEFLG("disabled", DISABLED);
	HANDLEFLG("pwnotreq", PWNOTREQ);
	HANDLEFLG("autolock", AUTOLOCK);
	HANDLEFLG("pwnoexp", PWNOEXP);

	if (argc == 0) {
		d_printf("%s's %s flag: %s\n", username, ctx->thiscmd, oldval);
		goto done;
	}

	ZERO_STRUCT(info->info21);

	info->info21.acct_flags = newflags;
	info->info21.fields_present = SAMR_FIELD_ACCT_FLAGS;

	result = rpccli_samr_SetUserInfo(pipe_hnd, mem_ctx,
					 user_hnd,
					 21,
					 info);

	if (NT_STATUS_IS_OK(result)) {
		d_printf("Set %s's %s flag from [%s] to [%s]\n", username,
			 ctx->thiscmd, oldval, argv[0]);
	}

 done:

	return result;
}

static NTSTATUS rpc_sh_user_flag_edit(TALLOC_CTX *mem_ctx,
				      struct rpc_sh_ctx *ctx,
				      struct rpc_pipe_client *pipe_hnd,
				      int argc, const char **argv)
{
	return rpc_sh_handle_user(mem_ctx, ctx, pipe_hnd, argc, argv,
				  rpc_sh_user_flag_edit_internals);
}

struct rpc_sh_cmd *net_rpc_user_edit_cmds(TALLOC_CTX *mem_ctx,
					  struct rpc_sh_ctx *ctx)
{
	static struct rpc_sh_cmd cmds[] = {

		{ "fullname", NULL, PI_SAMR, rpc_sh_user_str_edit,
		  "Show/Set a user's full name" },

		{ "homedir", NULL, PI_SAMR, rpc_sh_user_str_edit,
		  "Show/Set a user's home directory" },

		{ "homedrive", NULL, PI_SAMR, rpc_sh_user_str_edit,
		  "Show/Set a user's home drive" },

		{ "logonscript", NULL, PI_SAMR, rpc_sh_user_str_edit,
		  "Show/Set a user's logon script" },

		{ "profilepath", NULL, PI_SAMR, rpc_sh_user_str_edit,
		  "Show/Set a user's profile path" },

		{ "description", NULL, PI_SAMR, rpc_sh_user_str_edit,
		  "Show/Set a user's description" },

		{ "disabled", NULL, PI_SAMR, rpc_sh_user_flag_edit,
		  "Show/Set whether a user is disabled" },

		{ "autolock", NULL, PI_SAMR, rpc_sh_user_flag_edit,
		  "Show/Set whether a user locked out" },

		{ "pwnotreq", NULL, PI_SAMR, rpc_sh_user_flag_edit,
		  "Show/Set whether a user does not need a password" },

		{ "pwnoexp", NULL, PI_SAMR, rpc_sh_user_flag_edit,
		  "Show/Set whether a user's password does not expire" },

		{ NULL, NULL, 0, NULL, NULL }
	};

	return cmds;
}

struct rpc_sh_cmd *net_rpc_user_cmds(TALLOC_CTX *mem_ctx,
				     struct rpc_sh_ctx *ctx)
{
	static struct rpc_sh_cmd cmds[] = {

		{ "list", NULL, PI_SAMR, rpc_sh_user_list,
		  "List available users" },

		{ "info", NULL, PI_SAMR, rpc_sh_user_info,
		  "List the domain groups a user is member of" },

		{ "show", NULL, PI_SAMR, rpc_sh_user_show,
		  "Show info about a user" },

		{ "edit", net_rpc_user_edit_cmds, 0, NULL, 
		  "Show/Modify a user's fields" },

		{ NULL, NULL, 0, NULL, NULL }
	};

	return cmds;
}

/****************************************************************************/

/**
 * Basic usage function for 'net rpc group'.
 * @param argc	Standard main() style argc.
 * @param argv	Standard main() style argv. Initial components are already
 *		stripped.
 **/

static int rpc_group_usage(int argc, const char **argv)
{
	return net_help_group(argc, argv);
}

/**
 * Delete group on a remote RPC server.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through.
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/
                                                                                                             
static NTSTATUS rpc_group_delete_internals(const DOM_SID *domain_sid,
					const char *domain_name,
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	POLICY_HND connect_pol, domain_pol, group_pol, user_pol;
	bool group_is_primary = False;
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	uint32_t group_rid;
	struct samr_RidTypeArray *rids = NULL;
	/* char **names; */
	int i;
	/* DOM_GID *user_gids; */

	struct samr_Ids group_rids, name_types;
	struct lsa_String lsa_acct_name;
	union samr_UserInfo *info = NULL;

	if (argc < 1) {
        	d_printf("specify group\n");
		rpc_group_usage(argc,argv);
		return NT_STATUS_OK; /* ok? */
	}

	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);

        if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "Request samr_Connect2 failed\n");
        	goto done;
        }

        result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);

        if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "Request open_domain failed\n");
        	goto done;
        }

	init_lsa_String(&lsa_acct_name, argv[0]);

	result = rpccli_samr_LookupNames(pipe_hnd, mem_ctx,
					 &domain_pol,
					 1,
					 &lsa_acct_name,
					 &group_rids,
					 &name_types);
	if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "Lookup of '%s' failed\n",argv[0]);
   		goto done;
	}

	switch (name_types.ids[0])
	{
	case SID_NAME_DOM_GRP:
		result = rpccli_samr_OpenGroup(pipe_hnd, mem_ctx,
					       &domain_pol,
					       MAXIMUM_ALLOWED_ACCESS,
					       group_rids.ids[0],
					       &group_pol);
		if (!NT_STATUS_IS_OK(result)) {
			d_fprintf(stderr, "Request open_group failed");
   			goto done;
		}

		group_rid = group_rids.ids[0];

		result = rpccli_samr_QueryGroupMember(pipe_hnd, mem_ctx,
						      &group_pol,
						      &rids);

		if (!NT_STATUS_IS_OK(result)) {
			d_fprintf(stderr, "Unable to query group members of %s",argv[0]);
   			goto done;
		}
		
		if (opt_verbose) {
			d_printf("Domain Group %s (rid: %d) has %d members\n",
				argv[0],group_rid, rids->count);
		}

		/* Check if group is anyone's primary group */
                for (i = 0; i < rids->count; i++)
		{
	                result = rpccli_samr_OpenUser(pipe_hnd, mem_ctx,
						      &domain_pol,
						      MAXIMUM_ALLOWED_ACCESS,
						      rids->rids[i],
						      &user_pol);
	
	        	if (!NT_STATUS_IS_OK(result)) {
				d_fprintf(stderr, "Unable to open group member %d\n",
					rids->rids[i]);
	           		goto done;
	        	}

			result = rpccli_samr_QueryUserInfo(pipe_hnd, mem_ctx,
							   &user_pol,
							   21,
							   &info);

	        	if (!NT_STATUS_IS_OK(result)) {
				d_fprintf(stderr, "Unable to lookup userinfo for group member %d\n",
					rids->rids[i]);
	           		goto done;
	        	}

			if (info->info21.primary_gid == group_rid) {
				if (opt_verbose) {
					d_printf("Group is primary group of %s\n",
						info->info21.account_name.string);
				}
				group_is_primary = True;
                        }

			rpccli_samr_Close(pipe_hnd, mem_ctx, &user_pol);
		}
                
		if (group_is_primary) {
			d_fprintf(stderr, "Unable to delete group because some "
				 "of it's members have it as primary group\n");
			result = NT_STATUS_MEMBERS_PRIMARY_GROUP;
			goto done;
		}
     
		/* remove all group members */
		for (i = 0; i < rids->count; i++)
		{
			if (opt_verbose) 
				d_printf("Remove group member %d...",
					rids->rids[i]);
			result = rpccli_samr_DeleteGroupMember(pipe_hnd, mem_ctx,
							       &group_pol,
							       rids->rids[i]);

			if (NT_STATUS_IS_OK(result)) {
				if (opt_verbose)
					d_printf("ok\n");
			} else {
				if (opt_verbose)
					d_printf("failed\n");
				goto done;
			}	
		}

		result = rpccli_samr_DeleteDomainGroup(pipe_hnd, mem_ctx,
						       &group_pol);

		break;
	/* removing a local group is easier... */
	case SID_NAME_ALIAS:
		result = rpccli_samr_OpenAlias(pipe_hnd, mem_ctx,
					       &domain_pol,
					       MAXIMUM_ALLOWED_ACCESS,
					       group_rids.ids[0],
					       &group_pol);

		if (!NT_STATUS_IS_OK(result)) {
			d_fprintf(stderr, "Request open_alias failed\n");
   			goto done;
		}

		result = rpccli_samr_DeleteDomAlias(pipe_hnd, mem_ctx,
						    &group_pol);
		break;
	default:
		d_fprintf(stderr, "%s is of type %s. This command is only for deleting local or global groups\n",
			argv[0],sid_type_lookup(name_types.ids[0]));
		result = NT_STATUS_UNSUCCESSFUL;
		goto done;
	}
         
	
	if (NT_STATUS_IS_OK(result)) {
		if (opt_verbose)
			d_printf("Deleted %s '%s'\n",sid_type_lookup(name_types.ids[0]),argv[0]);
	} else {
		d_fprintf(stderr, "Deleting of %s failed: %s\n",argv[0],
			get_friendly_nt_error_msg(result));
	}
	
 done:
	return result;	
        
}

static int rpc_group_delete(int argc, const char **argv)
{
	return run_rpc_command(NULL, PI_SAMR, 0, rpc_group_delete_internals,
                               argc,argv);
}

static NTSTATUS rpc_group_add_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	POLICY_HND connect_pol, domain_pol, group_pol;
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	union samr_GroupInfo group_info;
	struct lsa_String grp_name;
	uint32_t rid = 0;

	if (argc != 1) {
		d_printf("Group name must be specified\n");
		rpc_group_usage(argc, argv);
		return NT_STATUS_OK;
	}

	init_lsa_String(&grp_name, argv[0]);

	/* Get sam policy handle */

	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);
	if (!NT_STATUS_IS_OK(result)) goto done;
	
	/* Get domain policy handle */

	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) goto done;

	/* Create the group */

	result = rpccli_samr_CreateDomainGroup(pipe_hnd, mem_ctx,
					       &domain_pol,
					       &grp_name,
					       MAXIMUM_ALLOWED_ACCESS,
					       &group_pol,
					       &rid);
	if (!NT_STATUS_IS_OK(result)) goto done;

	if (strlen(opt_comment) == 0) goto done;

	/* We've got a comment to set */

	init_lsa_String(&group_info.description, opt_comment);

	result = rpccli_samr_SetGroupInfo(pipe_hnd, mem_ctx,
					  &group_pol,
					  4,
					  &group_info);
	if (!NT_STATUS_IS_OK(result)) goto done;
	
 done:
	if (NT_STATUS_IS_OK(result))
		DEBUG(5, ("add group succeeded\n"));
	else
		d_fprintf(stderr, "add group failed: %s\n", nt_errstr(result));

	return result;
}

static NTSTATUS rpc_alias_add_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	POLICY_HND connect_pol, domain_pol, alias_pol;
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	union samr_AliasInfo alias_info;
	struct lsa_String alias_name;
	uint32_t rid = 0;

	if (argc != 1) {
		d_printf("Alias name must be specified\n");
		rpc_group_usage(argc, argv);
		return NT_STATUS_OK;
	}

	init_lsa_String(&alias_name, argv[0]);

	/* Get sam policy handle */

	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);
	if (!NT_STATUS_IS_OK(result)) goto done;
	
	/* Get domain policy handle */

	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) goto done;

	/* Create the group */

	result = rpccli_samr_CreateDomAlias(pipe_hnd, mem_ctx,
					    &domain_pol,
					    &alias_name,
					    MAXIMUM_ALLOWED_ACCESS,
					    &alias_pol,
					    &rid);
	if (!NT_STATUS_IS_OK(result)) goto done;

	if (strlen(opt_comment) == 0) goto done;

	/* We've got a comment to set */

	init_lsa_String(&alias_info.description, opt_comment);

	result = rpccli_samr_SetAliasInfo(pipe_hnd, mem_ctx,
					  &alias_pol,
					  3,
					  &alias_info);

	if (!NT_STATUS_IS_OK(result)) goto done;
	
 done:
	if (NT_STATUS_IS_OK(result))
		DEBUG(5, ("add alias succeeded\n"));
	else
		d_fprintf(stderr, "add alias failed: %s\n", nt_errstr(result));

	return result;
}

static int rpc_group_add(int argc, const char **argv)
{
	if (opt_localgroup)
		return run_rpc_command(NULL, PI_SAMR, 0,
				       rpc_alias_add_internals,
				       argc, argv);

	return run_rpc_command(NULL, PI_SAMR, 0,
			       rpc_group_add_internals,
			       argc, argv);
}

static NTSTATUS get_sid_from_name(struct cli_state *cli,
				TALLOC_CTX *mem_ctx,
				const char *name,
				DOM_SID *sid,
				enum lsa_SidType *type)
{
	DOM_SID *sids = NULL;
	enum lsa_SidType *types = NULL;
	struct rpc_pipe_client *pipe_hnd;
	POLICY_HND lsa_pol;
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;

	pipe_hnd = cli_rpc_pipe_open_noauth(cli, PI_LSARPC, &result);
	if (!pipe_hnd) {
		goto done;
	}

	result = rpccli_lsa_open_policy(pipe_hnd, mem_ctx, False,
				     SEC_RIGHTS_MAXIMUM_ALLOWED, &lsa_pol);

	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	result = rpccli_lsa_lookup_names(pipe_hnd, mem_ctx, &lsa_pol, 1,
				      &name, NULL, 1, &sids, &types);

	if (NT_STATUS_IS_OK(result)) {
		sid_copy(sid, &sids[0]);
		*type = types[0];
	}

	rpccli_lsa_Close(pipe_hnd, mem_ctx, &lsa_pol);

 done:
	if (pipe_hnd) {
		cli_rpc_pipe_close(pipe_hnd);
	}

	if (!NT_STATUS_IS_OK(result) && (StrnCaseCmp(name, "S-", 2) == 0)) {

		/* Try as S-1-5-whatever */

		DOM_SID tmp_sid;

		if (string_to_sid(&tmp_sid, name)) {
			sid_copy(sid, &tmp_sid);
			*type = SID_NAME_UNKNOWN;
			result = NT_STATUS_OK;
		}
	}

	return result;
}

static NTSTATUS rpc_add_groupmem(struct rpc_pipe_client *pipe_hnd,
				TALLOC_CTX *mem_ctx,
				const DOM_SID *group_sid,
				const char *member)
{
	POLICY_HND connect_pol, domain_pol;
	NTSTATUS result;
	uint32 group_rid;
	POLICY_HND group_pol;

	struct samr_Ids rids, rid_types;
	struct lsa_String lsa_acct_name;

	DOM_SID sid;

	sid_copy(&sid, group_sid);

	if (!sid_split_rid(&sid, &group_rid)) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	/* Get sam policy handle */
	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);
	if (!NT_STATUS_IS_OK(result)) {
		return result;
	}

	/* Get domain policy handle */
	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					&sid,
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) {
		return result;
	}

	init_lsa_String(&lsa_acct_name, member);

	result = rpccli_samr_LookupNames(pipe_hnd, mem_ctx,
					 &domain_pol,
					 1,
					 &lsa_acct_name,
					 &rids,
					 &rid_types);

	if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "Could not lookup up group member %s\n", member);
		goto done;
	}

	result = rpccli_samr_OpenGroup(pipe_hnd, mem_ctx,
				       &domain_pol,
				       MAXIMUM_ALLOWED_ACCESS,
				       group_rid,
				       &group_pol);

	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	result = rpccli_samr_AddGroupMember(pipe_hnd, mem_ctx,
					    &group_pol,
					    rids.ids[0],
					    0x0005); /* unknown flags */

 done:
	rpccli_samr_Close(pipe_hnd, mem_ctx, &connect_pol);
	return result;
}

static NTSTATUS rpc_add_aliasmem(struct rpc_pipe_client *pipe_hnd,
				TALLOC_CTX *mem_ctx,
				const DOM_SID *alias_sid,
				const char *member)
{
	POLICY_HND connect_pol, domain_pol;
	NTSTATUS result;
	uint32 alias_rid;
	POLICY_HND alias_pol;

	DOM_SID member_sid;
	enum lsa_SidType member_type;

	DOM_SID sid;

	sid_copy(&sid, alias_sid);

	if (!sid_split_rid(&sid, &alias_rid)) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	result = get_sid_from_name(pipe_hnd->cli, mem_ctx, member,
				   &member_sid, &member_type);

	if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "Could not lookup up group member %s\n", member);
		return result;
	}

	/* Get sam policy handle */
	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	/* Get domain policy handle */
	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					&sid,
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	result = rpccli_samr_OpenAlias(pipe_hnd, mem_ctx,
				       &domain_pol,
				       MAXIMUM_ALLOWED_ACCESS,
				       alias_rid,
				       &alias_pol);

	if (!NT_STATUS_IS_OK(result)) {
		return result;
	}

	result = rpccli_samr_AddAliasMember(pipe_hnd, mem_ctx,
					    &alias_pol,
					    &member_sid);

	if (!NT_STATUS_IS_OK(result)) {
		return result;
	}

 done:
	rpccli_samr_Close(pipe_hnd, mem_ctx, &connect_pol);
	return result;
}

static NTSTATUS rpc_group_addmem_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	DOM_SID group_sid;
	enum lsa_SidType group_type;

	if (argc != 2) {
		d_printf("Usage: 'net rpc group addmem <group> <member>\n");
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (!NT_STATUS_IS_OK(get_sid_from_name(cli, mem_ctx, argv[0],
					       &group_sid, &group_type))) {
		d_fprintf(stderr, "Could not lookup group name %s\n", argv[0]);
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (group_type == SID_NAME_DOM_GRP) {
		NTSTATUS result = rpc_add_groupmem(pipe_hnd, mem_ctx,
						   &group_sid, argv[1]);

		if (!NT_STATUS_IS_OK(result)) {
			d_fprintf(stderr, "Could not add %s to %s: %s\n",
				 argv[1], argv[0], nt_errstr(result));
		}
		return result;
	}

	if (group_type == SID_NAME_ALIAS) {
		NTSTATUS result = rpc_add_aliasmem(pipe_hnd, mem_ctx,
						   &group_sid, argv[1]);

		if (!NT_STATUS_IS_OK(result)) {
			d_fprintf(stderr, "Could not add %s to %s: %s\n",
				 argv[1], argv[0], nt_errstr(result));
		}
		return result;
	}

	d_fprintf(stderr, "Can only add members to global or local groups "
		 "which %s is not\n", argv[0]);

	return NT_STATUS_UNSUCCESSFUL;
}

static int rpc_group_addmem(int argc, const char **argv)
{
	return run_rpc_command(NULL, PI_SAMR, 0,
			       rpc_group_addmem_internals,
			       argc, argv);
}

static NTSTATUS rpc_del_groupmem(struct rpc_pipe_client *pipe_hnd,
				TALLOC_CTX *mem_ctx,
				const DOM_SID *group_sid,
				const char *member)
{
	POLICY_HND connect_pol, domain_pol;
	NTSTATUS result;
	uint32 group_rid;
	POLICY_HND group_pol;

	struct samr_Ids rids, rid_types;
	struct lsa_String lsa_acct_name;

	DOM_SID sid;

	sid_copy(&sid, group_sid);

	if (!sid_split_rid(&sid, &group_rid))
		return NT_STATUS_UNSUCCESSFUL;

	/* Get sam policy handle */
	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);
	if (!NT_STATUS_IS_OK(result))
		return result;

	/* Get domain policy handle */
	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					&sid,
					&domain_pol);
	if (!NT_STATUS_IS_OK(result))
		return result;

	init_lsa_String(&lsa_acct_name, member);

	result = rpccli_samr_LookupNames(pipe_hnd, mem_ctx,
					 &domain_pol,
					 1,
					 &lsa_acct_name,
					 &rids,
					 &rid_types);
	if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "Could not lookup up group member %s\n", member);
		goto done;
	}

	result = rpccli_samr_OpenGroup(pipe_hnd, mem_ctx,
				       &domain_pol,
				       MAXIMUM_ALLOWED_ACCESS,
				       group_rid,
				       &group_pol);

	if (!NT_STATUS_IS_OK(result))
		goto done;

	result = rpccli_samr_DeleteGroupMember(pipe_hnd, mem_ctx,
					       &group_pol,
					       rids.ids[0]);

 done:
	rpccli_samr_Close(pipe_hnd, mem_ctx, &connect_pol);
	return result;
}

static NTSTATUS rpc_del_aliasmem(struct rpc_pipe_client *pipe_hnd,
				TALLOC_CTX *mem_ctx,
				const DOM_SID *alias_sid,
				const char *member)
{
	POLICY_HND connect_pol, domain_pol;
	NTSTATUS result;
	uint32 alias_rid;
	POLICY_HND alias_pol;

	DOM_SID member_sid;
	enum lsa_SidType member_type;

	DOM_SID sid;

	sid_copy(&sid, alias_sid);

	if (!sid_split_rid(&sid, &alias_rid))
		return NT_STATUS_UNSUCCESSFUL;

	result = get_sid_from_name(pipe_hnd->cli, mem_ctx, member,
				   &member_sid, &member_type);

	if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "Could not lookup up group member %s\n", member);
		return result;
	}

	/* Get sam policy handle */
	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	/* Get domain policy handle */
	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					&sid,
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	result = rpccli_samr_OpenAlias(pipe_hnd, mem_ctx,
				       &domain_pol,
				       MAXIMUM_ALLOWED_ACCESS,
				       alias_rid,
				       &alias_pol);

	if (!NT_STATUS_IS_OK(result))
		return result;

	result = rpccli_samr_DeleteAliasMember(pipe_hnd, mem_ctx,
					       &alias_pol,
					       &member_sid);

	if (!NT_STATUS_IS_OK(result))
		return result;

 done:
	rpccli_samr_Close(pipe_hnd, mem_ctx, &connect_pol);
	return result;
}

static NTSTATUS rpc_group_delmem_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	DOM_SID group_sid;
	enum lsa_SidType group_type;

	if (argc != 2) {
		d_printf("Usage: 'net rpc group delmem <group> <member>\n");
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (!NT_STATUS_IS_OK(get_sid_from_name(cli, mem_ctx, argv[0],
					       &group_sid, &group_type))) {
		d_fprintf(stderr, "Could not lookup group name %s\n", argv[0]);
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (group_type == SID_NAME_DOM_GRP) {
		NTSTATUS result = rpc_del_groupmem(pipe_hnd, mem_ctx,
						   &group_sid, argv[1]);

		if (!NT_STATUS_IS_OK(result)) {
			d_fprintf(stderr, "Could not del %s from %s: %s\n",
				 argv[1], argv[0], nt_errstr(result));
		}
		return result;
	}

	if (group_type == SID_NAME_ALIAS) {
		NTSTATUS result = rpc_del_aliasmem(pipe_hnd, mem_ctx, 
						   &group_sid, argv[1]);

		if (!NT_STATUS_IS_OK(result)) {
			d_fprintf(stderr, "Could not del %s from %s: %s\n",
				 argv[1], argv[0], nt_errstr(result));
		}
		return result;
	}

	d_fprintf(stderr, "Can only delete members from global or local groups "
		 "which %s is not\n", argv[0]);

	return NT_STATUS_UNSUCCESSFUL;
}

static int rpc_group_delmem(int argc, const char **argv)
{
	return run_rpc_command(NULL, PI_SAMR, 0,
			       rpc_group_delmem_internals,
			       argc, argv);
}

/** 
 * List groups on a remote RPC server.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_group_list_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	POLICY_HND connect_pol, domain_pol;
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	uint32 start_idx=0, max_entries=250, num_entries, i, loop_count = 0;
	struct samr_SamArray *groups = NULL;
	bool global = False;
	bool local = False;
	bool builtin = False;

	if (argc == 0) {
		global = True;
		local = True;
		builtin = True;
	}

	for (i=0; i<argc; i++) {
		if (strequal(argv[i], "global"))
			global = True;

		if (strequal(argv[i], "local"))
			local = True;

		if (strequal(argv[i], "builtin"))
			builtin = True;
	}

	/* Get sam policy handle */

	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}
	
	/* Get domain policy handle */

	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	/* Query domain groups */
	if (opt_long_list_entries)
		d_printf("\nGroup name            Comment"\
			 "\n-----------------------------\n");
	do {
		uint32_t max_size, total_size, returned_size;
		union samr_DispInfo info;

		if (!global) break;

		get_query_dispinfo_params(
			loop_count, &max_entries, &max_size);

		result = rpccli_samr_QueryDisplayInfo(pipe_hnd, mem_ctx,
						      &domain_pol,
						      3,
						      start_idx,
						      max_entries,
						      max_size,
						      &total_size,
						      &returned_size,
						      &info);
		num_entries = info.info3.count;
		start_idx += info.info3.count;

		if (!NT_STATUS_IS_OK(result) &&
		    !NT_STATUS_EQUAL(result, STATUS_MORE_ENTRIES))
			break;

		for (i = 0; i < num_entries; i++) {

			const char *group = NULL;
			const char *desc = NULL;

			group = info.info3.entries[i].account_name.string;
			desc = info.info3.entries[i].description.string;

			if (opt_long_list_entries)
				printf("%-21.21s %-50.50s\n",
				       group, desc);
			else
				printf("%s\n", group);
		}
	} while (NT_STATUS_EQUAL(result, STATUS_MORE_ENTRIES));
	/* query domain aliases */
	start_idx = 0;
	do {
		if (!local) break;

		result = rpccli_samr_EnumDomainAliases(pipe_hnd, mem_ctx,
						       &domain_pol,
						       &start_idx,
						       &groups,
						       0xffff,
						       &num_entries);
		if (!NT_STATUS_IS_OK(result) &&
		    !NT_STATUS_EQUAL(result, STATUS_MORE_ENTRIES))
			break;

		for (i = 0; i < num_entries; i++) {

			const char *description = NULL;

			if (opt_long_list_entries) {

				POLICY_HND alias_pol;
				union samr_AliasInfo *info = NULL;

				if ((NT_STATUS_IS_OK(rpccli_samr_OpenAlias(pipe_hnd, mem_ctx,
									   &domain_pol,
									   0x8,
									   groups->entries[i].idx,
									   &alias_pol))) &&
				    (NT_STATUS_IS_OK(rpccli_samr_QueryAliasInfo(pipe_hnd, mem_ctx,
										&alias_pol,
										3,
										&info))) &&
				    (NT_STATUS_IS_OK(rpccli_samr_Close(pipe_hnd, mem_ctx,
								    &alias_pol)))) {
					description = info->description.string;
				}
			}

			if (description != NULL) {
				printf("%-21.21s %-50.50s\n",
				       groups->entries[i].name.string,
				       description);
			} else {
				printf("%s\n", groups->entries[i].name.string);
			}
		}
	} while (NT_STATUS_EQUAL(result, STATUS_MORE_ENTRIES));
	rpccli_samr_Close(pipe_hnd, mem_ctx, &domain_pol);
	/* Get builtin policy handle */

	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, &global_sid_Builtin),
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}
	/* query builtin aliases */
	start_idx = 0;
	do {
		if (!builtin) break;

		result = rpccli_samr_EnumDomainAliases(pipe_hnd, mem_ctx,
						       &domain_pol,
						       &start_idx,
						       &groups,
						       max_entries,
						       &num_entries);
		if (!NT_STATUS_IS_OK(result) &&
		    !NT_STATUS_EQUAL(result, STATUS_MORE_ENTRIES))
			break;

		for (i = 0; i < num_entries; i++) {

			const char *description = NULL;

			if (opt_long_list_entries) {

				POLICY_HND alias_pol;
				union samr_AliasInfo *info = NULL;

				if ((NT_STATUS_IS_OK(rpccli_samr_OpenAlias(pipe_hnd, mem_ctx,
									   &domain_pol,
									   0x8,
									   groups->entries[i].idx,
									   &alias_pol))) &&
				    (NT_STATUS_IS_OK(rpccli_samr_QueryAliasInfo(pipe_hnd, mem_ctx,
										&alias_pol,
										3,
										&info))) &&
				    (NT_STATUS_IS_OK(rpccli_samr_Close(pipe_hnd, mem_ctx,
								    &alias_pol)))) {
					description = info->description.string;
				}
			}

			if (description != NULL) {
				printf("%-21.21s %-50.50s\n",
				       groups->entries[i].name.string,
				       description);
			} else {
				printf("%s\n", groups->entries[i].name.string);
			}
		}
	} while (NT_STATUS_EQUAL(result, STATUS_MORE_ENTRIES));

 done:
	return result;
}

static int rpc_group_list(int argc, const char **argv)
{
	return run_rpc_command(NULL, PI_SAMR, 0,
			       rpc_group_list_internals,
			       argc, argv);
}

static NTSTATUS rpc_list_group_members(struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					const char *domain_name,
					const DOM_SID *domain_sid,
					POLICY_HND *domain_pol,
					uint32 rid)
{
	NTSTATUS result;
	POLICY_HND group_pol;
	uint32 num_members, *group_rids;
	int i;
	struct samr_RidTypeArray *rids = NULL;
	struct lsa_Strings names;
	struct samr_Ids types;

	fstring sid_str;
	sid_to_fstring(sid_str, domain_sid);

	result = rpccli_samr_OpenGroup(pipe_hnd, mem_ctx,
				       domain_pol,
				       MAXIMUM_ALLOWED_ACCESS,
				       rid,
				       &group_pol);

	if (!NT_STATUS_IS_OK(result))
		return result;

	result = rpccli_samr_QueryGroupMember(pipe_hnd, mem_ctx,
					      &group_pol,
					      &rids);

	if (!NT_STATUS_IS_OK(result))
		return result;

	num_members = rids->count;
	group_rids = rids->rids;

	while (num_members > 0) {
		int this_time = 512;

		if (num_members < this_time)
			this_time = num_members;

		result = rpccli_samr_LookupRids(pipe_hnd, mem_ctx,
						domain_pol,
						this_time,
						group_rids,
						&names,
						&types);

		if (!NT_STATUS_IS_OK(result))
			return result;

		/* We only have users as members, but make the output
		   the same as the output of alias members */

		for (i = 0; i < this_time; i++) {

			if (opt_long_list_entries) {
				printf("%s-%d %s\\%s %d\n", sid_str,
				       group_rids[i], domain_name,
				       names.names[i].string,
				       SID_NAME_USER);
			} else {
				printf("%s\\%s\n", domain_name,
					names.names[i].string);
			}
		}

		num_members -= this_time;
		group_rids += 512;
	}

	return NT_STATUS_OK;
}

static NTSTATUS rpc_list_alias_members(struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					POLICY_HND *domain_pol,
					uint32 rid)
{
	NTSTATUS result;
	struct rpc_pipe_client *lsa_pipe;
	POLICY_HND alias_pol, lsa_pol;
	uint32 num_members;
	DOM_SID *alias_sids;
	char **domains;
	char **names;
	enum lsa_SidType *types;
	int i;
	struct lsa_SidArray sid_array;

	result = rpccli_samr_OpenAlias(pipe_hnd, mem_ctx,
				       domain_pol,
				       MAXIMUM_ALLOWED_ACCESS,
				       rid,
				       &alias_pol);

	if (!NT_STATUS_IS_OK(result))
		return result;

	result = rpccli_samr_GetMembersInAlias(pipe_hnd, mem_ctx,
					       &alias_pol,
					       &sid_array);

	if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "Couldn't list alias members\n");
		return result;
	}

	num_members = sid_array.num_sids;

	if (num_members == 0) {
		return NT_STATUS_OK;
	}

	lsa_pipe = cli_rpc_pipe_open_noauth(pipe_hnd->cli, PI_LSARPC, &result);
	if (!lsa_pipe) {
		d_fprintf(stderr, "Couldn't open LSA pipe. Error was %s\n",
			nt_errstr(result) );
		return result;
	}

	result = rpccli_lsa_open_policy(lsa_pipe, mem_ctx, True,
				     SEC_RIGHTS_MAXIMUM_ALLOWED, &lsa_pol);

	if (!NT_STATUS_IS_OK(result)) {
		d_fprintf(stderr, "Couldn't open LSA policy handle\n");
		cli_rpc_pipe_close(lsa_pipe);
		return result;
	}

	alias_sids = TALLOC_ZERO_ARRAY(mem_ctx, DOM_SID, num_members);
	if (!alias_sids) {
		d_fprintf(stderr, "Out of memory\n");
		cli_rpc_pipe_close(lsa_pipe);
		return NT_STATUS_NO_MEMORY;
	}

	for (i=0; i<num_members; i++) {
		sid_copy(&alias_sids[i], sid_array.sids[i].sid);
	}

	result = rpccli_lsa_lookup_sids(lsa_pipe, mem_ctx, &lsa_pol, num_members,
				     alias_sids, 
				     &domains, &names, &types);

	if (!NT_STATUS_IS_OK(result) &&
	    !NT_STATUS_EQUAL(result, STATUS_SOME_UNMAPPED)) {
		d_fprintf(stderr, "Couldn't lookup SIDs\n");
		cli_rpc_pipe_close(lsa_pipe);
		return result;
	}

	for (i = 0; i < num_members; i++) {
		fstring sid_str;
		sid_to_fstring(sid_str, &alias_sids[i]);

		if (opt_long_list_entries) {
			printf("%s %s\\%s %d\n", sid_str, 
			       domains[i] ? domains[i] : "*unknown*", 
			       names[i] ? names[i] : "*unknown*", types[i]);
		} else {
			if (domains[i])
				printf("%s\\%s\n", domains[i], names[i]);
			else
				printf("%s\n", sid_str);
		}
	}

	cli_rpc_pipe_close(lsa_pipe);
	return NT_STATUS_OK;
}
 
static NTSTATUS rpc_group_members_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	NTSTATUS result;
	POLICY_HND connect_pol, domain_pol;
	struct samr_Ids rids, rid_types;
	struct lsa_String lsa_acct_name;

	/* Get sam policy handle */

	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);

	if (!NT_STATUS_IS_OK(result))
		return result;
	
	/* Get domain policy handle */

	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);

	if (!NT_STATUS_IS_OK(result))
		return result;

	init_lsa_String(&lsa_acct_name, argv[0]); /* sure? */

	result = rpccli_samr_LookupNames(pipe_hnd, mem_ctx,
					 &domain_pol,
					 1,
					 &lsa_acct_name,
					 &rids,
					 &rid_types);

	if (!NT_STATUS_IS_OK(result)) {

		/* Ok, did not find it in the global sam, try with builtin */

		DOM_SID sid_Builtin;

		rpccli_samr_Close(pipe_hnd, mem_ctx, &domain_pol);

		sid_copy(&sid_Builtin, &global_sid_Builtin);

		result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
						&connect_pol,
						MAXIMUM_ALLOWED_ACCESS,
						&sid_Builtin,
						&domain_pol);

		if (!NT_STATUS_IS_OK(result)) {
			d_fprintf(stderr, "Couldn't find group %s\n", argv[0]);
			return result;
		}

		result = rpccli_samr_LookupNames(pipe_hnd, mem_ctx,
						 &domain_pol,
						 1,
						 &lsa_acct_name,
						 &rids,
						 &rid_types);

		if (!NT_STATUS_IS_OK(result)) {
			d_fprintf(stderr, "Couldn't find group %s\n", argv[0]);
			return result;
		}
	}

	if (rids.count != 1) {
		d_fprintf(stderr, "Couldn't find group %s\n", argv[0]);
		return result;
	}

	if (rid_types.ids[0] == SID_NAME_DOM_GRP) {
		return rpc_list_group_members(pipe_hnd, mem_ctx, domain_name,
					      domain_sid, &domain_pol,
					      rids.ids[0]);
	}

	if (rid_types.ids[0] == SID_NAME_ALIAS) {
		return rpc_list_alias_members(pipe_hnd, mem_ctx, &domain_pol,
					      rids.ids[0]);
	}

	return NT_STATUS_NO_SUCH_GROUP;
}

static int rpc_group_members(int argc, const char **argv)
{
	if (argc != 1) {
		return rpc_group_usage(argc, argv);
	}

	return run_rpc_command(NULL, PI_SAMR, 0,
			       rpc_group_members_internals,
			       argc, argv);
}

static NTSTATUS rpc_group_rename_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	NTSTATUS result;
	POLICY_HND connect_pol, domain_pol, group_pol;
	union samr_GroupInfo group_info;
	struct samr_Ids rids, rid_types;
	struct lsa_String lsa_acct_name;

	if (argc != 2) {
		d_printf("Usage: 'net rpc group rename group newname'\n");
		return NT_STATUS_UNSUCCESSFUL;
	}

	/* Get sam policy handle */

	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);

	if (!NT_STATUS_IS_OK(result))
		return result;
	
	/* Get domain policy handle */

	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);

	if (!NT_STATUS_IS_OK(result))
		return result;

	init_lsa_String(&lsa_acct_name, argv[0]);

	result = rpccli_samr_LookupNames(pipe_hnd, mem_ctx,
					 &domain_pol,
					 1,
					 &lsa_acct_name,
					 &rids,
					 &rid_types);

	if (rids.count != 1) {
		d_fprintf(stderr, "Couldn't find group %s\n", argv[0]);
		return result;
	}

	if (rid_types.ids[0] != SID_NAME_DOM_GRP) {
		d_fprintf(stderr, "Can only rename domain groups\n");
		return NT_STATUS_UNSUCCESSFUL;
	}

	result = rpccli_samr_OpenGroup(pipe_hnd, mem_ctx,
				       &domain_pol,
				       MAXIMUM_ALLOWED_ACCESS,
				       rids.ids[0],
				       &group_pol);

	if (!NT_STATUS_IS_OK(result))
		return result;

	init_lsa_String(&group_info.name, argv[1]);

	result = rpccli_samr_SetGroupInfo(pipe_hnd, mem_ctx,
					  &group_pol,
					  2,
					  &group_info);

	if (!NT_STATUS_IS_OK(result))
		return result;

	return NT_STATUS_NO_SUCH_GROUP;
}

static int rpc_group_rename(int argc, const char **argv)
{
	if (argc != 2) {
		return rpc_group_usage(argc, argv);
	}

	return run_rpc_command(NULL, PI_SAMR, 0,
			       rpc_group_rename_internals,
			       argc, argv);
}

/** 
 * 'net rpc group' entrypoint.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/

int net_rpc_group(int argc, const char **argv) 
{
	struct functable func[] = {
		{"add", rpc_group_add},
		{"delete", rpc_group_delete},
		{"addmem", rpc_group_addmem},
		{"delmem", rpc_group_delmem},
		{"list", rpc_group_list},
		{"members", rpc_group_members},
		{"rename", rpc_group_rename},
		{NULL, NULL}
	};
	
	if (argc == 0) {
		return run_rpc_command(NULL, PI_SAMR, 0, 
				       rpc_group_list_internals,
				       argc, argv);
	}

	return net_run_function(argc, argv, func, rpc_group_usage);
}

/****************************************************************************/

static int rpc_share_usage(int argc, const char **argv)
{
	return net_help_share(argc, argv);
}

/** 
 * Add a share on a remote RPC server.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/
static NTSTATUS rpc_share_add_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,int argc,
					const char **argv)
{
	WERROR result;
	NTSTATUS status;
	char *sharename;
	char *path;
	uint32 type = STYPE_DISKTREE; /* only allow disk shares to be added */
	uint32 num_users=0, perms=0;
	char *password=NULL; /* don't allow a share password */
	uint32 level = 2;
	union srvsvc_NetShareInfo info;
	struct srvsvc_NetShareInfo2 info2;
	uint32_t parm_error = 0;

	if ((sharename = talloc_strdup(mem_ctx, argv[0])) == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	path = strchr(sharename, '=');
	if (!path)
		return NT_STATUS_UNSUCCESSFUL;
	*path++ = '\0';

	info2.name		= sharename;
	info2.type		= type;
	info2.comment		= opt_comment;
	info2.permissions	= perms;
	info2.max_users		= opt_maxusers;
	info2.current_users	= num_users;
	info2.path		= path;
	info2.password		= password;

	info.info2 = &info2;

	status = rpccli_srvsvc_NetShareAdd(pipe_hnd, mem_ctx,
					   pipe_hnd->cli->desthost,
					   level,
					   &info,
					   &parm_error,
					   &result);
	return status;
}

static int rpc_share_add(int argc, const char **argv)
{
	if ((argc < 1) || !strchr(argv[0], '=')) {
		DEBUG(1,("Sharename or path not specified on add\n"));
		return rpc_share_usage(argc, argv);
	}
	return run_rpc_command(NULL, PI_SRVSVC, 0, 
			       rpc_share_add_internals,
			       argc, argv);
}

/** 
 * Delete a share on a remote RPC server.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/
static NTSTATUS rpc_share_del_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	WERROR result;

	return rpccli_srvsvc_NetShareDel(pipe_hnd, mem_ctx,
					 pipe_hnd->cli->desthost,
					 argv[0],
					 0,
					 &result);
}

/** 
 * Delete a share on a remote RPC server.
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_share_delete(int argc, const char **argv)
{
	if (argc < 1) {
		DEBUG(1,("Sharename not specified on delete\n"));
		return rpc_share_usage(argc, argv);
	}
	return run_rpc_command(NULL, PI_SRVSVC, 0, 
			       rpc_share_del_internals,
			       argc, argv);
}

/**
 * Formatted print of share info
 *
 * @param info1  pointer to SRV_SHARE_INFO_1 to format
 **/

static void display_share_info_1(struct srvsvc_NetShareInfo1 *r)
{
	if (opt_long_list_entries) {
		d_printf("%-12s %-8.8s %-50s\n",
			 r->name,
			 share_type[r->type & ~(STYPE_TEMPORARY|STYPE_HIDDEN)],
			 r->comment);
	} else {
		d_printf("%s\n", r->name);
	}
}

static WERROR get_share_info(struct rpc_pipe_client *pipe_hnd,
			     TALLOC_CTX *mem_ctx,
			     uint32 level,
			     int argc,
			     const char **argv,
			     struct srvsvc_NetShareInfoCtr *info_ctr)
{
	WERROR result;
	NTSTATUS status;
	union srvsvc_NetShareInfo info;

	/* no specific share requested, enumerate all */
	if (argc == 0) {

		uint32_t preferred_len = 0xffffffff;
		uint32_t total_entries = 0;
		uint32_t resume_handle = 0;

		info_ctr->level = level;

		status = rpccli_srvsvc_NetShareEnumAll(pipe_hnd, mem_ctx,
						       pipe_hnd->cli->desthost,
						       info_ctr,
						       preferred_len,
						       &total_entries,
						       &resume_handle,
						       &result);
		return result;
	}

	/* request just one share */
	status = rpccli_srvsvc_NetShareGetInfo(pipe_hnd, mem_ctx,
					       pipe_hnd->cli->desthost,
					       argv[0],
					       level,
					       &info,
					       &result);

	if (!NT_STATUS_IS_OK(status) || !W_ERROR_IS_OK(result)) {
		goto done;
	}

	/* construct ctr */
	ZERO_STRUCTP(info_ctr);

	info_ctr->level = level;

	switch (level) {
	case 1:
	{
		struct srvsvc_NetShareCtr1 *ctr1;

		ctr1 = TALLOC_ZERO_P(mem_ctx, struct srvsvc_NetShareCtr1);
		W_ERROR_HAVE_NO_MEMORY(ctr1);

		ctr1->count = 1;
		ctr1->array = info.info1;

		info_ctr->ctr.ctr1 = ctr1;
	}
	case 2:
	{
		struct srvsvc_NetShareCtr2 *ctr2;

		ctr2 = TALLOC_ZERO_P(mem_ctx, struct srvsvc_NetShareCtr2);
		W_ERROR_HAVE_NO_MEMORY(ctr2);

		ctr2->count = 1;
		ctr2->array = info.info2;

		info_ctr->ctr.ctr2 = ctr2;
	}
	case 502:
	{
		struct srvsvc_NetShareCtr502 *ctr502;

		ctr502 = TALLOC_ZERO_P(mem_ctx, struct srvsvc_NetShareCtr502);
		W_ERROR_HAVE_NO_MEMORY(ctr502);

		ctr502->count = 1;
		ctr502->array = info.info502;

		info_ctr->ctr.ctr502 = ctr502;
	}
	} /* switch */
done:
	return result;
}

/** 
 * List shares on a remote RPC server.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_share_list_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	struct srvsvc_NetShareInfoCtr info_ctr;
	struct srvsvc_NetShareCtr1 ctr1;
	WERROR result;
	uint32 i, level = 1;

	ZERO_STRUCT(info_ctr);
	ZERO_STRUCT(ctr1);

	info_ctr.level = 1;
	info_ctr.ctr.ctr1 = &ctr1;

	result = get_share_info(pipe_hnd, mem_ctx, level, argc, argv, &info_ctr);
	if (!W_ERROR_IS_OK(result))
		goto done;

	/* Display results */

	if (opt_long_list_entries) {
		d_printf(
	"\nEnumerating shared resources (exports) on remote server:\n\n"\
	"\nShare name   Type     Description\n"\
	"----------   ----     -----------\n");
	}
	for (i = 0; i < info_ctr.ctr.ctr1->count; i++)
		display_share_info_1(&info_ctr.ctr.ctr1->array[i]);
 done:
	return W_ERROR_IS_OK(result) ? NT_STATUS_OK : NT_STATUS_UNSUCCESSFUL;
}

/*** 
 * 'net rpc share list' entrypoint.
 * @param argc  Standard main() style argc
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/
static int rpc_share_list(int argc, const char **argv)
{
	return run_rpc_command(NULL, PI_SRVSVC, 0, rpc_share_list_internals, argc, argv);
}

static bool check_share_availability(struct cli_state *cli, const char *netname)
{
	if (!cli_send_tconX(cli, netname, "A:", "", 0)) {
		d_printf("skipping   [%s]: not a file share.\n", netname);
		return False;
	}

	if (!cli_tdis(cli)) 
		return False;

	return True;
}

static bool check_share_sanity(struct cli_state *cli, const char *netname, uint32 type)
{
	/* only support disk shares */
	if (! ( type == STYPE_DISKTREE || type == (STYPE_DISKTREE | STYPE_HIDDEN)) ) {
		printf("share [%s] is not a diskshare (type: %x)\n", netname, type);
		return False;
	}

	/* skip builtin shares */
	/* FIXME: should print$ be added too ? */
	if (strequal(netname,"IPC$") || strequal(netname,"ADMIN$") || 
	    strequal(netname,"global")) 
		return False;

	if (opt_exclude && in_list(netname, opt_exclude, False)) {
		printf("excluding  [%s]\n", netname);
		return False;
	}

	return check_share_availability(cli, netname);
}

/** 
 * Migrate shares from a remote RPC server to the local RPC server.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_share_migrate_shares_internals(const DOM_SID *domain_sid,
						const char *domain_name, 
						struct cli_state *cli,
						struct rpc_pipe_client *pipe_hnd,
						TALLOC_CTX *mem_ctx, 
						int argc,
						const char **argv)
{
	WERROR result;
	NTSTATUS nt_status = NT_STATUS_UNSUCCESSFUL;
	struct srvsvc_NetShareInfoCtr ctr_src;
	uint32 i;
	struct rpc_pipe_client *srvsvc_pipe = NULL;
	struct cli_state *cli_dst = NULL;
	uint32 level = 502; /* includes secdesc */
	uint32_t parm_error = 0;

	result = get_share_info(pipe_hnd, mem_ctx, level, argc, argv, &ctr_src);
	if (!W_ERROR_IS_OK(result))
		goto done;

	/* connect destination PI_SRVSVC */
        nt_status = connect_dst_pipe(&cli_dst, &srvsvc_pipe, PI_SRVSVC);
        if (!NT_STATUS_IS_OK(nt_status))
                return nt_status;


	for (i = 0; i < ctr_src.ctr.ctr502->count; i++) {

		union srvsvc_NetShareInfo info;
		struct srvsvc_NetShareInfo502 info502 =
			ctr_src.ctr.ctr502->array[i];

		/* reset error-code */
		nt_status = NT_STATUS_UNSUCCESSFUL;

		if (!check_share_sanity(cli, info502.name, info502.type))
			continue;

		/* finally add the share on the dst server */ 

		printf("migrating: [%s], path: %s, comment: %s, without share-ACLs\n", 
			info502.name, info502.path, info502.comment);

		info.info502 = &info502;

		nt_status = rpccli_srvsvc_NetShareAdd(srvsvc_pipe, mem_ctx,
						      srvsvc_pipe->cli->desthost,
						      502,
						      &info,
						      &parm_error,
						      &result);

                if (W_ERROR_V(result) == W_ERROR_V(WERR_ALREADY_EXISTS)) {
			printf("           [%s] does already exist\n",
				info502.name);
			continue;
		}

		if (!NT_STATUS_IS_OK(nt_status) || !W_ERROR_IS_OK(result)) {
			printf("cannot add share: %s\n", dos_errstr(result));
			goto done;
		}

	}

	nt_status = NT_STATUS_OK;

done:
	if (cli_dst) {
		cli_shutdown(cli_dst);
	}

	return nt_status;

}

/** 
 * Migrate shares from a rpc-server to another.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_share_migrate_shares(int argc, const char **argv)
{

	if (!opt_host) {
		printf("no server to migrate\n");
		return -1;
	}

	return run_rpc_command(NULL, PI_SRVSVC, 0, 
			       rpc_share_migrate_shares_internals,
			       argc, argv);
}

/**
 * Copy a file/dir 
 *
 * @param f	file_info
 * @param mask	current search mask
 * @param state	arg-pointer
 *
 **/
static void copy_fn(const char *mnt, file_info *f, const char *mask, void *state)
{
	static NTSTATUS nt_status;
	static struct copy_clistate *local_state;
	static fstring filename, new_mask;
	fstring dir;
	char *old_dir;

	local_state = (struct copy_clistate *)state;
	nt_status = NT_STATUS_UNSUCCESSFUL;

	if (strequal(f->name, ".") || strequal(f->name, ".."))
		return;

	DEBUG(3,("got mask: %s, name: %s\n", mask, f->name));

	/* DIRECTORY */
	if (f->mode & aDIR) {

		DEBUG(3,("got dir: %s\n", f->name));

		fstrcpy(dir, local_state->cwd);
		fstrcat(dir, "\\");
		fstrcat(dir, f->name);

		switch (net_mode_share)
		{
		case NET_MODE_SHARE_MIGRATE:
			/* create that directory */
			nt_status = net_copy_file(local_state->mem_ctx,
						  local_state->cli_share_src,
						  local_state->cli_share_dst,
						  dir, dir,
						  opt_acls? True : False,
						  opt_attrs? True : False,
						  opt_timestamps? True : False,
						  False);
			break;
		default:
			d_fprintf(stderr, "Unsupported mode %d\n", net_mode_share);
			return;
		}

		if (!NT_STATUS_IS_OK(nt_status)) 
			printf("could not handle dir %s: %s\n", 
				dir, nt_errstr(nt_status));

		/* search below that directory */
		fstrcpy(new_mask, dir);
		fstrcat(new_mask, "\\*");

		old_dir = local_state->cwd;
		local_state->cwd = dir;
		if (!sync_files(local_state, new_mask))
			printf("could not handle files\n");
		local_state->cwd = old_dir;

		return;
	}


	/* FILE */
	fstrcpy(filename, local_state->cwd);
	fstrcat(filename, "\\");
	fstrcat(filename, f->name);

	DEBUG(3,("got file: %s\n", filename));

	switch (net_mode_share)
	{
	case NET_MODE_SHARE_MIGRATE:
		nt_status = net_copy_file(local_state->mem_ctx, 
					  local_state->cli_share_src, 
					  local_state->cli_share_dst, 
					  filename, filename, 
					  opt_acls? True : False, 
					  opt_attrs? True : False,
					  opt_timestamps? True: False,
					  True);
		break;
	default:
		d_fprintf(stderr, "Unsupported file mode %d\n", net_mode_share);
		return;
	}

	if (!NT_STATUS_IS_OK(nt_status)) 
		printf("could not handle file %s: %s\n", 
			filename, nt_errstr(nt_status));

}

/**
 * sync files, can be called recursivly to list files 
 * and then call copy_fn for each file 
 *
 * @param cp_clistate	pointer to the copy_clistate we work with
 * @param mask		the current search mask
 *
 * @return 		Boolean result
 **/
static bool sync_files(struct copy_clistate *cp_clistate, const char *mask)
{
	struct cli_state *targetcli;
	char *targetpath = NULL;

	DEBUG(3,("calling cli_list with mask: %s\n", mask));

	if ( !cli_resolve_path(talloc_tos(), "", cp_clistate->cli_share_src,
				mask, &targetcli, &targetpath ) ) {
		d_fprintf(stderr, "cli_resolve_path %s failed with error: %s\n", 
			mask, cli_errstr(cp_clistate->cli_share_src));
		return False;
	}

	if (cli_list(targetcli, targetpath, cp_clistate->attribute, copy_fn, cp_clistate) == -1) {
		d_fprintf(stderr, "listing %s failed with error: %s\n", 
			mask, cli_errstr(targetcli));
		return False;
	}

	return True;
}


/**
 * Set the top level directory permissions before we do any further copies.
 * Should set up ACL inheritance.
 **/

bool copy_top_level_perms(struct copy_clistate *cp_clistate, 
				const char *sharename)
{
	NTSTATUS nt_status = NT_STATUS_UNSUCCESSFUL;

	switch (net_mode_share) {
	case NET_MODE_SHARE_MIGRATE:
		DEBUG(3,("calling net_copy_fileattr for '.' directory in share %s\n", sharename));
		nt_status = net_copy_fileattr(cp_clistate->mem_ctx,
						cp_clistate->cli_share_src, 
						cp_clistate->cli_share_dst,
						"\\", "\\",
						opt_acls? True : False, 
						opt_attrs? True : False,
						opt_timestamps? True: False,
						False);
		break;
	default:
		d_fprintf(stderr, "Unsupported mode %d\n", net_mode_share);
		break;
	}

	if (!NT_STATUS_IS_OK(nt_status))  {
		printf("Could handle directory attributes for top level directory of share %s. Error %s\n", 
			sharename, nt_errstr(nt_status));
		return False;
	}

	return True;
}

/** 
 * Sync all files inside a remote share to another share (over smb).
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_share_migrate_files_internals(const DOM_SID *domain_sid,
						const char *domain_name, 
						struct cli_state *cli,
						struct rpc_pipe_client *pipe_hnd,
						TALLOC_CTX *mem_ctx,
						int argc,
						const char **argv)
{
	WERROR result;
	NTSTATUS nt_status = NT_STATUS_UNSUCCESSFUL;
	struct srvsvc_NetShareInfoCtr ctr_src;
	uint32 i;
	uint32 level = 502;
	struct copy_clistate cp_clistate;
	bool got_src_share = False;
	bool got_dst_share = False;
	const char *mask = "\\*";
	char *dst = NULL;

	dst = SMB_STRDUP(opt_destination?opt_destination:"127.0.0.1");
	if (dst == NULL) {
		nt_status = NT_STATUS_NO_MEMORY;
		goto done;
	}

	result = get_share_info(pipe_hnd, mem_ctx, level, argc, argv, &ctr_src);

	if (!W_ERROR_IS_OK(result))
		goto done;

	for (i = 0; i < ctr_src.ctr.ctr502->count; i++) {

		struct srvsvc_NetShareInfo502 info502 =
			ctr_src.ctr.ctr502->array[i];

		if (!check_share_sanity(cli, info502.name, info502.type))
			continue;

		/* one might not want to mirror whole discs :) */
		if (strequal(info502.name, "print$") || info502.name[1] == '$') {
			d_printf("skipping   [%s]: builtin/hidden share\n", info502.name);
			continue;
		}

		switch (net_mode_share)
		{
		case NET_MODE_SHARE_MIGRATE:
			printf("syncing");
			break;
		default:
			d_fprintf(stderr, "Unsupported mode %d\n", net_mode_share);
			break;
		}
		printf("    [%s] files and directories %s ACLs, %s DOS Attributes %s\n", 
			info502.name,
			opt_acls ? "including" : "without", 
			opt_attrs ? "including" : "without",
			opt_timestamps ? "(preserving timestamps)" : "");

		cp_clistate.mem_ctx = mem_ctx;
		cp_clistate.cli_share_src = NULL;
		cp_clistate.cli_share_dst = NULL;
		cp_clistate.cwd = NULL;
		cp_clistate.attribute = aSYSTEM | aHIDDEN | aDIR;

	        /* open share source */
		nt_status = connect_to_service(&cp_clistate.cli_share_src,
					       &cli->dest_ss, cli->desthost,
					       info502.name, "A:");
		if (!NT_STATUS_IS_OK(nt_status))
			goto done;

		got_src_share = True;

		if (net_mode_share == NET_MODE_SHARE_MIGRATE) {
			/* open share destination */
			nt_status = connect_to_service(&cp_clistate.cli_share_dst,
						       NULL, dst, info502.name, "A:");
			if (!NT_STATUS_IS_OK(nt_status))
				goto done;

			got_dst_share = True;
		}

		if (!copy_top_level_perms(&cp_clistate, info502.name)) {
			d_fprintf(stderr, "Could not handle the top level directory permissions for the share: %s\n", info502.name);
			nt_status = NT_STATUS_UNSUCCESSFUL;
			goto done;
		}

		if (!sync_files(&cp_clistate, mask)) {
			d_fprintf(stderr, "could not handle files for share: %s\n", info502.name);
			nt_status = NT_STATUS_UNSUCCESSFUL;
			goto done;
		}
	}

	nt_status = NT_STATUS_OK;

done:

	if (got_src_share)
		cli_shutdown(cp_clistate.cli_share_src);

	if (got_dst_share)
		cli_shutdown(cp_clistate.cli_share_dst);

	SAFE_FREE(dst);
	return nt_status;

}

static int rpc_share_migrate_files(int argc, const char **argv)
{

	if (!opt_host) {
		printf("no server to migrate\n");
		return -1;
	}

	return run_rpc_command(NULL, PI_SRVSVC, 0, 
			       rpc_share_migrate_files_internals,
			       argc, argv);
}

/** 
 * Migrate share-ACLs from a remote RPC server to the local RPC server.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_share_migrate_security_internals(const DOM_SID *domain_sid,
						const char *domain_name, 
						struct cli_state *cli,
						struct rpc_pipe_client *pipe_hnd,
						TALLOC_CTX *mem_ctx, 
						int argc,
						const char **argv)
{
	WERROR result;
	NTSTATUS nt_status = NT_STATUS_UNSUCCESSFUL;
	struct srvsvc_NetShareInfoCtr ctr_src;
	union srvsvc_NetShareInfo info;
	uint32 i;
	struct rpc_pipe_client *srvsvc_pipe = NULL;
	struct cli_state *cli_dst = NULL;
	uint32 level = 502; /* includes secdesc */
	uint32_t parm_error = 0;

	result = get_share_info(pipe_hnd, mem_ctx, level, argc, argv, &ctr_src);

	if (!W_ERROR_IS_OK(result))
		goto done;

	/* connect destination PI_SRVSVC */
        nt_status = connect_dst_pipe(&cli_dst, &srvsvc_pipe, PI_SRVSVC);
        if (!NT_STATUS_IS_OK(nt_status))
                return nt_status;


	for (i = 0; i < ctr_src.ctr.ctr502->count; i++) {

		struct srvsvc_NetShareInfo502 info502 =
			ctr_src.ctr.ctr502->array[i];

		/* reset error-code */
		nt_status = NT_STATUS_UNSUCCESSFUL;

		if (!check_share_sanity(cli, info502.name, info502.type))
			continue;

		printf("migrating: [%s], path: %s, comment: %s, including share-ACLs\n", 
			info502.name, info502.path, info502.comment);

		if (opt_verbose)
			display_sec_desc(info502.sd_buf.sd);

		/* FIXME: shouldn't we be able to just set the security descriptor ? */
		info.info502 = &info502;

		/* finally modify the share on the dst server */
		nt_status = rpccli_srvsvc_NetShareSetInfo(srvsvc_pipe, mem_ctx,
							  srvsvc_pipe->cli->desthost,
							  info502.name,
							  level,
							  &info,
							  &parm_error,
							  &result);
		if (!NT_STATUS_IS_OK(nt_status) || !W_ERROR_IS_OK(result)) {
			printf("cannot set share-acl: %s\n", dos_errstr(result));
			goto done;
		}

	}

	nt_status = NT_STATUS_OK;

done:
	if (cli_dst) {
		cli_shutdown(cli_dst);
	}

	return nt_status;

}

/** 
 * Migrate share-acls from a rpc-server to another.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success)
 **/
static int rpc_share_migrate_security(int argc, const char **argv)
{

	if (!opt_host) {
		printf("no server to migrate\n");
		return -1;
	}

	return run_rpc_command(NULL, PI_SRVSVC, 0, 
			       rpc_share_migrate_security_internals,
			       argc, argv);
}

/** 
 * Migrate shares (including share-definitions, share-acls and files with acls/attrs)
 * from one server to another
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success)
 *
 **/
static int rpc_share_migrate_all(int argc, const char **argv)
{
	int ret;

	if (!opt_host) {
		printf("no server to migrate\n");
		return -1;
	}

	/* order is important. we don't want to be locked out by the share-acl
	 * before copying files - gd */
	
	ret = run_rpc_command(NULL, PI_SRVSVC, 0, rpc_share_migrate_shares_internals, argc, argv);
	if (ret)
		return ret;

	ret = run_rpc_command(NULL, PI_SRVSVC, 0, rpc_share_migrate_files_internals, argc, argv);
	if (ret)
		return ret;
	
	return run_rpc_command(NULL, PI_SRVSVC, 0, rpc_share_migrate_security_internals, argc, argv);
}


/** 
 * 'net rpc share migrate' entrypoint.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/
static int rpc_share_migrate(int argc, const char **argv)
{

	struct functable func[] = {
		{"all", 	rpc_share_migrate_all},
		{"files", 	rpc_share_migrate_files},
		{"help",	rpc_share_usage},
		{"security", 	rpc_share_migrate_security},
		{"shares", 	rpc_share_migrate_shares},
		{NULL, NULL}
	};

	net_mode_share = NET_MODE_SHARE_MIGRATE;

	return net_run_function(argc, argv, func, rpc_share_usage);
}

struct full_alias {
	DOM_SID sid;
	uint32 num_members;
	DOM_SID *members;
};

static int num_server_aliases;
static struct full_alias *server_aliases;

/*
 * Add an alias to the static list.
 */
static void push_alias(TALLOC_CTX *mem_ctx, struct full_alias *alias)
{
	if (server_aliases == NULL)
		server_aliases = SMB_MALLOC_ARRAY(struct full_alias, 100);

	server_aliases[num_server_aliases] = *alias;
	num_server_aliases += 1;
}

/*
 * For a specific domain on the server, fetch all the aliases
 * and their members. Add all of them to the server_aliases.
 */

static NTSTATUS rpc_fetch_domain_aliases(struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					POLICY_HND *connect_pol,
					const DOM_SID *domain_sid)
{
	uint32 start_idx, max_entries, num_entries, i;
	struct samr_SamArray *groups = NULL;
	NTSTATUS result;
	POLICY_HND domain_pol;

	/* Get domain policy handle */

	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);
	if (!NT_STATUS_IS_OK(result))
		return result;

	start_idx = 0;
	max_entries = 250;

	do {
		result = rpccli_samr_EnumDomainAliases(pipe_hnd, mem_ctx,
						       &domain_pol,
						       &start_idx,
						       &groups,
						       max_entries,
						       &num_entries);
		for (i = 0; i < num_entries; i++) {

			POLICY_HND alias_pol;
			struct full_alias alias;
			struct lsa_SidArray sid_array;
			int j;

			result = rpccli_samr_OpenAlias(pipe_hnd, mem_ctx,
						       &domain_pol,
						       MAXIMUM_ALLOWED_ACCESS,
						       groups->entries[i].idx,
						       &alias_pol);
			if (!NT_STATUS_IS_OK(result))
				goto done;

			result = rpccli_samr_GetMembersInAlias(pipe_hnd, mem_ctx,
							       &alias_pol,
							       &sid_array);
			if (!NT_STATUS_IS_OK(result))
				goto done;

			alias.num_members = sid_array.num_sids;

			result = rpccli_samr_Close(pipe_hnd, mem_ctx, &alias_pol);
			if (!NT_STATUS_IS_OK(result))
				goto done;

			alias.members = NULL;

			if (alias.num_members > 0) {
				alias.members = SMB_MALLOC_ARRAY(DOM_SID, alias.num_members);

				for (j = 0; j < alias.num_members; j++)
					sid_copy(&alias.members[j],
						 sid_array.sids[j].sid);
			}

			sid_copy(&alias.sid, domain_sid);
			sid_append_rid(&alias.sid, groups->entries[i].idx);

			push_alias(mem_ctx, &alias);
		}
	} while (NT_STATUS_EQUAL(result, STATUS_MORE_ENTRIES));

	result = NT_STATUS_OK;

 done:
	rpccli_samr_Close(pipe_hnd, mem_ctx, &domain_pol);

	return result;
}

/*
 * Dump server_aliases as names for debugging purposes.
 */

static NTSTATUS rpc_aliaslist_dump(const DOM_SID *domain_sid,
				const char *domain_name,
				struct cli_state *cli,
				struct rpc_pipe_client *pipe_hnd,
				TALLOC_CTX *mem_ctx, 
				int argc,
				const char **argv)
{
	int i;
	NTSTATUS result;
	POLICY_HND lsa_pol;

	result = rpccli_lsa_open_policy(pipe_hnd, mem_ctx, True, 
				     SEC_RIGHTS_MAXIMUM_ALLOWED,
				     &lsa_pol);
	if (!NT_STATUS_IS_OK(result))
		return result;

	for (i=0; i<num_server_aliases; i++) {
		char **names;
		char **domains;
		enum lsa_SidType *types;
		int j;

		struct full_alias *alias = &server_aliases[i];

		result = rpccli_lsa_lookup_sids(pipe_hnd, mem_ctx, &lsa_pol, 1,
					     &alias->sid,
					     &domains, &names, &types);
		if (!NT_STATUS_IS_OK(result))
			continue;

		DEBUG(1, ("%s\\%s %d: ", domains[0], names[0], types[0]));

		if (alias->num_members == 0) {
			DEBUG(1, ("\n"));
			continue;
		}

		result = rpccli_lsa_lookup_sids(pipe_hnd, mem_ctx, &lsa_pol,
					     alias->num_members,
					     alias->members,
					     &domains, &names, &types);

		if (!NT_STATUS_IS_OK(result) &&
		    !NT_STATUS_EQUAL(result, STATUS_SOME_UNMAPPED))
			continue;

		for (j=0; j<alias->num_members; j++)
			DEBUG(1, ("%s\\%s (%d); ",
				  domains[j] ? domains[j] : "*unknown*", 
				  names[j] ? names[j] : "*unknown*",types[j]));
		DEBUG(1, ("\n"));
	}

	rpccli_lsa_Close(pipe_hnd, mem_ctx, &lsa_pol);

	return NT_STATUS_OK;
}

/*
 * Fetch a list of all server aliases and their members into
 * server_aliases.
 */

static NTSTATUS rpc_aliaslist_internals(const DOM_SID *domain_sid,
					const char *domain_name,
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx, 
					int argc,
					const char **argv)
{
	NTSTATUS result;
	POLICY_HND connect_pol;

	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);

	if (!NT_STATUS_IS_OK(result))
		goto done;
	
	result = rpc_fetch_domain_aliases(pipe_hnd, mem_ctx, &connect_pol,
					  &global_sid_Builtin);

	if (!NT_STATUS_IS_OK(result))
		goto done;
	
	result = rpc_fetch_domain_aliases(pipe_hnd, mem_ctx, &connect_pol,
					  domain_sid);

	rpccli_samr_Close(pipe_hnd, mem_ctx, &connect_pol);
 done:
	return result;
}

static void init_user_token(NT_USER_TOKEN *token, DOM_SID *user_sid)
{
	token->num_sids = 4;

	if (!(token->user_sids = SMB_MALLOC_ARRAY(DOM_SID, 4))) {
		d_fprintf(stderr, "malloc failed\n");
		token->num_sids = 0;
		return;
	}

	token->user_sids[0] = *user_sid;
	sid_copy(&token->user_sids[1], &global_sid_World);
	sid_copy(&token->user_sids[2], &global_sid_Network);
	sid_copy(&token->user_sids[3], &global_sid_Authenticated_Users);
}

static void free_user_token(NT_USER_TOKEN *token)
{
	SAFE_FREE(token->user_sids);
}

static bool is_sid_in_token(NT_USER_TOKEN *token, DOM_SID *sid)
{
	int i;

	for (i=0; i<token->num_sids; i++) {
		if (sid_compare(sid, &token->user_sids[i]) == 0)
			return True;
	}
	return False;
}

static void add_sid_to_token(NT_USER_TOKEN *token, DOM_SID *sid)
{
	if (is_sid_in_token(token, sid))
		return;

	token->user_sids = SMB_REALLOC_ARRAY(token->user_sids, DOM_SID, token->num_sids+1);
	if (!token->user_sids) {
		return;
	}

	sid_copy(&token->user_sids[token->num_sids], sid);

	token->num_sids += 1;
}

struct user_token {
	fstring name;
	NT_USER_TOKEN token;
};

static void dump_user_token(struct user_token *token)
{
	int i;

	d_printf("%s\n", token->name);

	for (i=0; i<token->token.num_sids; i++) {
		d_printf(" %s\n", sid_string_tos(&token->token.user_sids[i]));
	}
}

static bool is_alias_member(DOM_SID *sid, struct full_alias *alias)
{
	int i;

	for (i=0; i<alias->num_members; i++) {
		if (sid_compare(sid, &alias->members[i]) == 0)
			return True;
	}

	return False;
}

static void collect_sid_memberships(NT_USER_TOKEN *token, DOM_SID sid)
{
	int i;

	for (i=0; i<num_server_aliases; i++) {
		if (is_alias_member(&sid, &server_aliases[i]))
			add_sid_to_token(token, &server_aliases[i].sid);
	}
}

/*
 * We got a user token with all the SIDs we can know about without asking the
 * server directly. These are the user and domain group sids. All of these can
 * be members of aliases. So scan the list of aliases for each of the SIDs and
 * add them to the token.
 */

static void collect_alias_memberships(NT_USER_TOKEN *token)
{
	int num_global_sids = token->num_sids;
	int i;

	for (i=0; i<num_global_sids; i++) {
		collect_sid_memberships(token, token->user_sids[i]);
	}
}

static bool get_user_sids(const char *domain, const char *user, NT_USER_TOKEN *token)
{
	wbcErr wbc_status = WBC_ERR_UNKNOWN_FAILURE;
	enum wbcSidType type;
	fstring full_name;
	struct wbcDomainSid wsid;
	char *sid_str = NULL;
	DOM_SID user_sid;
	uint32_t num_groups;
	gid_t *groups = NULL;
	uint32_t i;

	fstr_sprintf(full_name, "%s%c%s",
		     domain, *lp_winbind_separator(), user);

	/* First let's find out the user sid */

	wbc_status = wbcLookupName(domain, user, &wsid, &type);

	if (!WBC_ERROR_IS_OK(wbc_status)) {
		DEBUG(1, ("winbind could not find %s: %s\n",
			  full_name, wbcErrorString(wbc_status)));
		return false;
	}

	wbc_status = wbcSidToString(&wsid, &sid_str);
	if (!WBC_ERROR_IS_OK(wbc_status)) {
		return false;
	}

	if (type != SID_NAME_USER) {
		wbcFreeMemory(sid_str);
		DEBUG(1, ("%s is not a user\n", full_name));
		return false;
	}

	string_to_sid(&user_sid, sid_str);
	wbcFreeMemory(sid_str);
	sid_str = NULL;

	init_user_token(token, &user_sid);

	/* And now the groups winbind knows about */

	wbc_status = wbcGetGroups(full_name, &num_groups, &groups);
	if (!WBC_ERROR_IS_OK(wbc_status)) {
		DEBUG(1, ("winbind could not get groups of %s: %s\n",
			full_name, wbcErrorString(wbc_status)));
		return false;
	}

	for (i = 0; i < num_groups; i++) {
		gid_t gid = groups[i];
		DOM_SID sid;

		wbc_status = wbcGidToSid(gid, &wsid);
		if (!WBC_ERROR_IS_OK(wbc_status)) {
			DEBUG(1, ("winbind could not find SID of gid %d: %s\n",
				  gid, wbcErrorString(wbc_status)));
			wbcFreeMemory(groups);
			return false;
		}

		wbc_status = wbcSidToString(&wsid, &sid_str);
		if (!WBC_ERROR_IS_OK(wbc_status)) {
			wbcFreeMemory(groups);
			return false;
		}

		DEBUG(3, (" %s\n", sid_str));

		string_to_sid(&sid, sid_str);
		wbcFreeMemory(sid_str);
		sid_str = NULL;

		add_sid_to_token(token, &sid);
	}
	wbcFreeMemory(groups);

	return true;
}
	
/**
 * Get a list of all user tokens we want to look at
 **/

static bool get_user_tokens(int *num_tokens, struct user_token **user_tokens)
{
	wbcErr wbc_status = WBC_ERR_UNKNOWN_FAILURE;
	uint32_t i, num_users;
	const char **users;
	struct user_token *result;
	TALLOC_CTX *frame = NULL;

	if (lp_winbind_use_default_domain() &&
	    (opt_target_workgroup == NULL)) {
		d_fprintf(stderr, "winbind use default domain = yes set, "
			 "please specify a workgroup\n");
		return false;
	}

	/* Send request to winbind daemon */

	wbc_status = wbcListUsers(NULL, &num_users, &users);
	if (!WBC_ERROR_IS_OK(wbc_status)) {
		DEBUG(1, ("winbind could not list users: %s\n",
			  wbcErrorString(wbc_status)));
		return false;
	}

	result = SMB_MALLOC_ARRAY(struct user_token, num_users);

	if (result == NULL) {
		DEBUG(1, ("Could not malloc sid array\n"));
		wbcFreeMemory(users);
		return false;
	}

	frame = talloc_stackframe();
	for (i=0; i < num_users; i++) {
		fstring domain, user;
		char *p;

		fstrcpy(result[i].name, users[i]);

		p = strchr(users[i], *lp_winbind_separator());

		DEBUG(3, ("%s\n", users[i]));

		if (p == NULL) {
			fstrcpy(domain, opt_target_workgroup);
			fstrcpy(user, users[i]);
		} else {
			*p++ = '\0';
			fstrcpy(domain, users[i]);
			strupper_m(domain);
			fstrcpy(user, p);
		}

		get_user_sids(domain, user, &(result[i].token));
		i+=1;
	}
	TALLOC_FREE(frame);
	wbcFreeMemory(users);

	*num_tokens = num_users;
	*user_tokens = result;

	return true;
}

static bool get_user_tokens_from_file(FILE *f,
				      int *num_tokens,
				      struct user_token **tokens)
{
	struct user_token *token = NULL;

	while (!feof(f)) {
		fstring line;

		if (fgets(line, sizeof(line)-1, f) == NULL) {
			return True;
		}

		if (line[strlen(line)-1] == '\n')
			line[strlen(line)-1] = '\0';

		if (line[0] == ' ') {
			/* We have a SID */

			DOM_SID sid;
			string_to_sid(&sid, &line[1]);

			if (token == NULL) {
				DEBUG(0, ("File does not begin with username"));
				return False;
			}

			add_sid_to_token(&token->token, &sid);
			continue;
		}

		/* And a new user... */

		*num_tokens += 1;
		*tokens = SMB_REALLOC_ARRAY(*tokens, struct user_token, *num_tokens);
		if (*tokens == NULL) {
			DEBUG(0, ("Could not realloc tokens\n"));
			return False;
		}

		token = &((*tokens)[*num_tokens-1]);

		fstrcpy(token->name, line);
		token->token.num_sids = 0;
		token->token.user_sids = NULL;
		continue;
	}
	
	return False;
}


/*
 * Show the list of all users that have access to a share
 */

static void show_userlist(struct rpc_pipe_client *pipe_hnd,
			TALLOC_CTX *mem_ctx,
			const char *netname,
			int num_tokens,
			struct user_token *tokens)
{
	int fnum;
	SEC_DESC *share_sd = NULL;
	SEC_DESC *root_sd = NULL;
	struct cli_state *cli = pipe_hnd->cli;
	int i;
	union srvsvc_NetShareInfo info;
	WERROR result;
	NTSTATUS status;
	uint16 cnum;

	status = rpccli_srvsvc_NetShareGetInfo(pipe_hnd, mem_ctx,
					       pipe_hnd->cli->desthost,
					       netname,
					       502,
					       &info,
					       &result);

	if (!NT_STATUS_IS_OK(status) || !W_ERROR_IS_OK(result)) {
		DEBUG(1, ("Coult not query secdesc for share %s\n",
			  netname));
		return;
	}

	share_sd = info.info502->sd_buf.sd;
	if (share_sd == NULL) {
		DEBUG(1, ("Got no secdesc for share %s\n",
			  netname));
	}

	cnum = cli->cnum;

	if (!cli_send_tconX(cli, netname, "A:", "", 0)) {
		return;
	}

	fnum = cli_nt_create(cli, "\\", READ_CONTROL_ACCESS);

	if (fnum != -1) {
		root_sd = cli_query_secdesc(cli, fnum, mem_ctx);
	}

	for (i=0; i<num_tokens; i++) {
		uint32 acc_granted;

		if (share_sd != NULL) {
			if (!se_access_check(share_sd, &tokens[i].token,
					     1, &acc_granted, &status)) {
				DEBUG(1, ("Could not check share_sd for "
					  "user %s\n",
					  tokens[i].name));
				continue;
			}

			if (!NT_STATUS_IS_OK(status))
				continue;
		}

		if (root_sd == NULL) {
			d_printf(" %s\n", tokens[i].name);
			continue;
		}

		if (!se_access_check(root_sd, &tokens[i].token,
				     1, &acc_granted, &status)) {
			DEBUG(1, ("Could not check root_sd for user %s\n",
				  tokens[i].name));
			continue;
		}

		if (!NT_STATUS_IS_OK(status))
			continue;

		d_printf(" %s\n", tokens[i].name);
	}

	if (fnum != -1)
		cli_close(cli, fnum);
	cli_tdis(cli);
	cli->cnum = cnum;
	
	return;
}

struct share_list {
	int num_shares;
	char **shares;
};

static void collect_share(const char *name, uint32 m,
			  const char *comment, void *state)
{
	struct share_list *share_list = (struct share_list *)state;

	if (m != STYPE_DISKTREE)
		return;

	share_list->num_shares += 1;
	share_list->shares = SMB_REALLOC_ARRAY(share_list->shares, char *, share_list->num_shares);
	if (!share_list->shares) {
		share_list->num_shares = 0;
		return;
	}
	share_list->shares[share_list->num_shares-1] = SMB_STRDUP(name);
}

static void rpc_share_userlist_usage(void)
{
	return;
}
	
/** 
 * List shares on a remote RPC server, including the security descriptors.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_share_allowedusers_internals(const DOM_SID *domain_sid,
						const char *domain_name,
						struct cli_state *cli,
						struct rpc_pipe_client *pipe_hnd,
						TALLOC_CTX *mem_ctx,
						int argc,
						const char **argv)
{
	int ret;
	bool r;
	ENUM_HND hnd;
	uint32 i;
	FILE *f;

	struct user_token *tokens = NULL;
	int num_tokens = 0;

	struct share_list share_list;

	if (argc > 1) {
		rpc_share_userlist_usage();
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (argc == 0) {
		f = stdin;
	} else {
		f = fopen(argv[0], "r");
	}

	if (f == NULL) {
		DEBUG(0, ("Could not open userlist: %s\n", strerror(errno)));
		return NT_STATUS_UNSUCCESSFUL;
	}

	r = get_user_tokens_from_file(f, &num_tokens, &tokens);

	if (f != stdin)
		fclose(f);

	if (!r) {
		DEBUG(0, ("Could not read users from file\n"));
		return NT_STATUS_UNSUCCESSFUL;
	}

	for (i=0; i<num_tokens; i++)
		collect_alias_memberships(&tokens[i].token);

	init_enum_hnd(&hnd, 0);

	share_list.num_shares = 0;
	share_list.shares = NULL;

	ret = cli_RNetShareEnum(cli, collect_share, &share_list);

	if (ret == -1) {
		DEBUG(0, ("Error returning browse list: %s\n",
			  cli_errstr(cli)));
		goto done;
	}

	for (i = 0; i < share_list.num_shares; i++) {
		char *netname = share_list.shares[i];

		if (netname[strlen(netname)-1] == '$')
			continue;

		d_printf("%s\n", netname);

		show_userlist(pipe_hnd, mem_ctx, netname,
			      num_tokens, tokens);
	}
 done:
	for (i=0; i<num_tokens; i++) {
		free_user_token(&tokens[i].token);
	}
	SAFE_FREE(tokens);
	SAFE_FREE(share_list.shares);

	return NT_STATUS_OK;
}

static int rpc_share_allowedusers(int argc, const char **argv)
{
	int result;

	result = run_rpc_command(NULL, PI_SAMR, 0,
				 rpc_aliaslist_internals,
				 argc, argv);
	if (result != 0)
		return result;

	result = run_rpc_command(NULL, PI_LSARPC, 0,
				 rpc_aliaslist_dump,
				 argc, argv);
	if (result != 0)
		return result;

	return run_rpc_command(NULL, PI_SRVSVC, 0,
			       rpc_share_allowedusers_internals,
			       argc, argv);
}

int net_usersidlist(int argc, const char **argv)
{
	int num_tokens = 0;
	struct user_token *tokens = NULL;
	int i;

	if (argc != 0) {
		net_usersidlist_usage(argc, argv);
		return 0;
	}

	if (!get_user_tokens(&num_tokens, &tokens)) {
		DEBUG(0, ("Could not get the user/sid list\n"));
		return 0;
	}

	for (i=0; i<num_tokens; i++) {
		dump_user_token(&tokens[i]);
		free_user_token(&tokens[i].token);
	}

	SAFE_FREE(tokens);
	return 1;
}

int net_usersidlist_usage(int argc, const char **argv)
{
	d_printf("net usersidlist\n"
		 "\tprints out a list of all users the running winbind knows\n"
		 "\tabout, together with all their SIDs. This is used as\n"
		 "\tinput to the 'net rpc share allowedusers' command.\n\n");

	net_common_flags_usage(argc, argv);
	return -1;
}

/** 
 * 'net rpc share' entrypoint.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/

int net_rpc_share(int argc, const char **argv) 
{
	struct functable func[] = {
		{"add", rpc_share_add},
		{"delete", rpc_share_delete},
		{"allowedusers", rpc_share_allowedusers},
		{"migrate", rpc_share_migrate},
		{"list", rpc_share_list},
		{NULL, NULL}
	};

	if (argc == 0)
		return run_rpc_command(NULL, PI_SRVSVC, 0, 
				       rpc_share_list_internals,
				       argc, argv);

	return net_run_function(argc, argv, func, rpc_share_usage);
}

static NTSTATUS rpc_sh_share_list(TALLOC_CTX *mem_ctx,
				  struct rpc_sh_ctx *ctx,
				  struct rpc_pipe_client *pipe_hnd,
				  int argc, const char **argv)
{
	return rpc_share_list_internals(ctx->domain_sid, ctx->domain_name,
					ctx->cli, pipe_hnd, mem_ctx,
					argc, argv);
}

static NTSTATUS rpc_sh_share_add(TALLOC_CTX *mem_ctx,
				 struct rpc_sh_ctx *ctx,
				 struct rpc_pipe_client *pipe_hnd,
				 int argc, const char **argv)
{
	WERROR result;
	NTSTATUS status;
	uint32_t parm_err = 0;
	union srvsvc_NetShareInfo info;
	struct srvsvc_NetShareInfo2 info2;

	if ((argc < 2) || (argc > 3)) {
		d_fprintf(stderr, "usage: %s <share> <path> [comment]\n",
			  ctx->whoami);
		return NT_STATUS_INVALID_PARAMETER;
	}

	info2.name		= argv[0];
	info2.type		= STYPE_DISKTREE;
	info2.comment		= (argc == 3) ? argv[2] : "";
	info2.permissions	= 0;
	info2.max_users		= 0;
	info2.current_users	= 0;
	info2.path		= argv[1];
	info2.password		= NULL;

	info.info2 = &info2;

	status = rpccli_srvsvc_NetShareAdd(pipe_hnd, mem_ctx,
					   pipe_hnd->cli->desthost,
					   2,
					   &info,
					   &parm_err,
					   &result);

	return status;
}

static NTSTATUS rpc_sh_share_delete(TALLOC_CTX *mem_ctx,
				    struct rpc_sh_ctx *ctx,
				    struct rpc_pipe_client *pipe_hnd,
				    int argc, const char **argv)
{
	WERROR result;
	NTSTATUS status;

	if (argc != 1) {
		d_fprintf(stderr, "usage: %s <share>\n", ctx->whoami);
		return NT_STATUS_INVALID_PARAMETER;
	}

	status = rpccli_srvsvc_NetShareDel(pipe_hnd, mem_ctx,
					   pipe_hnd->cli->desthost,
					   argv[0],
					   0,
					   &result);

	return status;
}

static NTSTATUS rpc_sh_share_info(TALLOC_CTX *mem_ctx,
				  struct rpc_sh_ctx *ctx,
				  struct rpc_pipe_client *pipe_hnd,
				  int argc, const char **argv)
{
	union srvsvc_NetShareInfo info;
	WERROR result;
	NTSTATUS status;

	if (argc != 1) {
		d_fprintf(stderr, "usage: %s <share>\n", ctx->whoami);
		return NT_STATUS_INVALID_PARAMETER;
	}

	status = rpccli_srvsvc_NetShareGetInfo(pipe_hnd, mem_ctx,
					       pipe_hnd->cli->desthost,
					       argv[0],
					       2,
					       &info,
					       &result);
	if (!NT_STATUS_IS_OK(status) || !W_ERROR_IS_OK(result)) {
		goto done;
	}

	d_printf("Name:     %s\n", info.info2->name);
	d_printf("Comment:  %s\n", info.info2->comment);
	d_printf("Path:     %s\n", info.info2->path);
	d_printf("Password: %s\n", info.info2->password);

 done:
	return werror_to_ntstatus(result);
}

struct rpc_sh_cmd *net_rpc_share_cmds(TALLOC_CTX *mem_ctx,
				      struct rpc_sh_ctx *ctx)
{
	static struct rpc_sh_cmd cmds[] = {

	{ "list", NULL, PI_SRVSVC, rpc_sh_share_list,
	  "List available shares" },

	{ "add", NULL, PI_SRVSVC, rpc_sh_share_add,
	  "Add a share" },

	{ "delete", NULL, PI_SRVSVC, rpc_sh_share_delete,
	  "Delete a share" },

	{ "info", NULL, PI_SRVSVC, rpc_sh_share_info,
	  "Get information about a share" },

	{ NULL, NULL, 0, NULL, NULL }
	};

	return cmds;
}

/****************************************************************************/

static int rpc_file_usage(int argc, const char **argv)
{
	return net_help_file(argc, argv);
}

/** 
 * Close a file on a remote RPC server.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/
static NTSTATUS rpc_file_close_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	return rpccli_srvsvc_NetFileClose(pipe_hnd, mem_ctx, 
					    pipe_hnd->cli->desthost, 
					    atoi(argv[0]), NULL);
}

/** 
 * Close a file on a remote RPC server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_file_close(int argc, const char **argv)
{
	if (argc < 1) {
		DEBUG(1, ("No fileid given on close\n"));
		return(rpc_file_usage(argc, argv));
	}

	return run_rpc_command(NULL, PI_SRVSVC, 0, 
			       rpc_file_close_internals,
			       argc, argv);
}

/** 
 * Formatted print of open file info 
 *
 * @param r  struct srvsvc_NetFileInfo3 contents
 **/

static void display_file_info_3(struct srvsvc_NetFileInfo3 *r)
{
	d_printf("%-7.1d %-20.20s 0x%-4.2x %-6.1d %s\n",
		 r->fid, r->user, r->permissions, r->num_locks, r->path);
}

/** 
 * List open files on a remote RPC server.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_file_list_internals(const DOM_SID *domain_sid,
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	struct srvsvc_NetFileInfoCtr info_ctr;
	struct srvsvc_NetFileCtr3 ctr3;
	WERROR result;
	NTSTATUS status;
	uint32 preferred_len = 0xffffffff, i;
	const char *username=NULL;
	uint32_t total_entries = 0;
	uint32_t resume_handle = 0;

	/* if argc > 0, must be user command */
	if (argc > 0)
		username = smb_xstrdup(argv[0]);

	ZERO_STRUCT(info_ctr);
	ZERO_STRUCT(ctr3);

	info_ctr.level = 3;
	info_ctr.ctr.ctr3 = &ctr3;

	status = rpccli_srvsvc_NetFileEnum(pipe_hnd, mem_ctx,
					   pipe_hnd->cli->desthost,
					   NULL,
					   username,
					   &info_ctr,
					   preferred_len,
					   &total_entries,
					   &resume_handle,
					   &result);

	if (!NT_STATUS_IS_OK(status) || !W_ERROR_IS_OK(result))
		goto done;

	/* Display results */

	d_printf(
		 "\nEnumerating open files on remote server:\n\n"\
		 "\nFileId  Opened by            Perms  Locks  Path"\
		 "\n------  ---------            -----  -----  ---- \n");
	for (i = 0; i < total_entries; i++)
		display_file_info_3(&info_ctr.ctr.ctr3->array[i]);
 done:
	return W_ERROR_IS_OK(result) ? NT_STATUS_OK : NT_STATUS_UNSUCCESSFUL;
}

/** 
 * List files for a user on a remote RPC server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/

static int rpc_file_user(int argc, const char **argv)
{
	if (argc < 1) {
		DEBUG(1, ("No username given\n"));
		return(rpc_file_usage(argc, argv));
	}

	return run_rpc_command(NULL, PI_SRVSVC, 0, 
			       rpc_file_list_internals,
			       argc, argv);
}

/** 
 * 'net rpc file' entrypoint.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/

int net_rpc_file(int argc, const char **argv) 
{
	struct functable func[] = {
		{"close", rpc_file_close},
		{"user", rpc_file_user},
#if 0
		{"info", rpc_file_info},
#endif
		{NULL, NULL}
	};

	if (argc == 0)
		return run_rpc_command(NULL, PI_SRVSVC, 0, 
				       rpc_file_list_internals,
				       argc, argv);

	return net_run_function(argc, argv, func, rpc_file_usage);
}

/** 
 * ABORT the shutdown of a remote RPC Server, over initshutdown pipe.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_shutdown_abort_internals(const DOM_SID *domain_sid, 
					const char *domain_name, 
					struct cli_state *cli, 
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx, 
					int argc,
					const char **argv) 
{
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	
	result = rpccli_initshutdown_Abort(pipe_hnd, mem_ctx, NULL, NULL);
	
	if (NT_STATUS_IS_OK(result)) {
		d_printf("\nShutdown successfully aborted\n");
		DEBUG(5,("cmd_shutdown_abort: query succeeded\n"));
	} else
		DEBUG(5,("cmd_shutdown_abort: query failed\n"));
	
	return result;
}

/** 
 * ABORT the shutdown of a remote RPC Server, over winreg pipe.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

static NTSTATUS rpc_reg_shutdown_abort_internals(const DOM_SID *domain_sid, 
						const char *domain_name, 
						struct cli_state *cli, 
						struct rpc_pipe_client *pipe_hnd,
						TALLOC_CTX *mem_ctx, 
						int argc,
						const char **argv) 
{
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	
	result = rpccli_winreg_AbortSystemShutdown(pipe_hnd, mem_ctx, NULL, NULL);
	
	if (NT_STATUS_IS_OK(result)) {
		d_printf("\nShutdown successfully aborted\n");
		DEBUG(5,("cmd_reg_abort_shutdown: query succeeded\n"));
	} else
		DEBUG(5,("cmd_reg_abort_shutdown: query failed\n"));
	
	return result;
}

/** 
 * ABORT the Shut down of a remote RPC server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/

static int rpc_shutdown_abort(int argc, const char **argv) 
{
	int rc = run_rpc_command(NULL, PI_INITSHUTDOWN, 0, 
				 rpc_shutdown_abort_internals,
				 argc, argv);

	if (rc == 0)
		return rc;

	DEBUG(1, ("initshutdown pipe didn't work, trying winreg pipe\n"));

	return run_rpc_command(NULL, PI_WINREG, 0, 
			       rpc_reg_shutdown_abort_internals,
			       argc, argv);
}

/** 
 * Shut down a remote RPC Server via initshutdown pipe.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

NTSTATUS rpc_init_shutdown_internals(const DOM_SID *domain_sid,
						const char *domain_name, 
						struct cli_state *cli, 
						struct rpc_pipe_client *pipe_hnd,
						TALLOC_CTX *mem_ctx, 
						int argc,
						const char **argv) 
{
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
        const char *msg = "This machine will be shutdown shortly";
	uint32 timeout = 20;
	struct initshutdown_String msg_string;
	struct initshutdown_String_sub s;

	if (opt_comment) {
		msg = opt_comment;
	}
	if (opt_timeout) {
		timeout = opt_timeout;
	}

	s.name = msg;
	msg_string.name = &s;

	/* create an entry */
	result = rpccli_initshutdown_Init(pipe_hnd, mem_ctx, NULL,
			&msg_string, timeout, opt_force, opt_reboot, NULL);

	if (NT_STATUS_IS_OK(result)) {
		d_printf("\nShutdown of remote machine succeeded\n");
		DEBUG(5,("Shutdown of remote machine succeeded\n"));
	} else {
		DEBUG(1,("Shutdown of remote machine failed!\n"));
	}
	return result;
}

/** 
 * Shut down a remote RPC Server via winreg pipe.
 *
 * All parameters are provided by the run_rpc_command function, except for
 * argc, argv which are passed through. 
 *
 * @param domain_sid The domain sid acquired from the remote server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return Normal NTSTATUS return.
 **/

NTSTATUS rpc_reg_shutdown_internals(const DOM_SID *domain_sid,
						const char *domain_name, 
						struct cli_state *cli, 
						struct rpc_pipe_client *pipe_hnd,
						TALLOC_CTX *mem_ctx, 
						int argc,
						const char **argv) 
{
        const char *msg = "This machine will be shutdown shortly";
	uint32 timeout = 20;
	struct initshutdown_String msg_string;
	struct initshutdown_String_sub s;
	NTSTATUS result;
	WERROR werr;

	if (opt_comment) {
		msg = opt_comment;
	}
	s.name = msg;
	msg_string.name = &s;

	if (opt_timeout) {
		timeout = opt_timeout;
	}

	/* create an entry */
	result = rpccli_winreg_InitiateSystemShutdown(pipe_hnd, mem_ctx, NULL,
			&msg_string, timeout, opt_force, opt_reboot, &werr);

	if (NT_STATUS_IS_OK(result)) {
		d_printf("\nShutdown of remote machine succeeded\n");
	} else {
		d_fprintf(stderr, "\nShutdown of remote machine failed\n");
		if ( W_ERROR_EQUAL(werr, WERR_MACHINE_LOCKED) )
			d_fprintf(stderr, "\nMachine locked, use -f switch to force\n");
		else
			d_fprintf(stderr, "\nresult was: %s\n", dos_errstr(werr));
	}

	return result;
}

/** 
 * Shut down a remote RPC server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/

static int rpc_shutdown(int argc, const char **argv) 
{
	int rc = run_rpc_command(NULL, PI_INITSHUTDOWN, 0, 
				 rpc_init_shutdown_internals,
				 argc, argv);

	if (rc) {
		DEBUG(1, ("initshutdown pipe failed, trying winreg pipe\n"));
		rc = run_rpc_command(NULL, PI_WINREG, 0, 
				     rpc_reg_shutdown_internals, argc, argv);
	}

	return rc;
}

/***************************************************************************
  NT Domain trusts code (i.e. 'net rpc trustdom' functionality)
  
 ***************************************************************************/

/**
 * Add interdomain trust account to the RPC server.
 * All parameters (except for argc and argv) are passed by run_rpc_command
 * function.
 *
 * @param domain_sid The domain sid acquired from the server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return normal NTSTATUS return code.
 */

static NTSTATUS rpc_trustdom_add_internals(const DOM_SID *domain_sid, 
						const char *domain_name, 
						struct cli_state *cli,
						struct rpc_pipe_client *pipe_hnd,
						TALLOC_CTX *mem_ctx, 
						int argc,
						const char **argv)
{
	POLICY_HND connect_pol, domain_pol, user_pol;
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	char *acct_name;
	struct lsa_String lsa_acct_name;
	uint32 acb_info;
	uint32 acct_flags=0;
	uint32 user_rid;
	uint32_t access_granted = 0;
	union samr_UserInfo info;
	unsigned int orig_timeout;

	if (argc != 2) {
		d_printf("Usage: net rpc trustdom add <domain_name> <trust password>\n");
		return NT_STATUS_INVALID_PARAMETER;
	}

	/* 
	 * Make valid trusting domain account (ie. uppercased and with '$' appended)
	 */

	if (asprintf(&acct_name, "%s$", argv[0]) < 0) {
		return NT_STATUS_NO_MEMORY;
	}

	strupper_m(acct_name);

	init_lsa_String(&lsa_acct_name, acct_name);

	/* Get samr policy handle */
	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	/* Get domain policy handle */
	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

        /* This call can take a long time - allow the server to time out.
	 * 35 seconds should do it. */

        orig_timeout = cli_set_timeout(pipe_hnd->cli, 35000);

	/* Create trusting domain's account */
	acb_info = ACB_NORMAL;
	acct_flags = SEC_GENERIC_READ | SEC_GENERIC_WRITE | SEC_GENERIC_EXECUTE |
		     SEC_STD_WRITE_DAC | SEC_STD_DELETE |
		     SAMR_USER_ACCESS_SET_PASSWORD |
		     SAMR_USER_ACCESS_GET_ATTRIBUTES |
		     SAMR_USER_ACCESS_SET_ATTRIBUTES;

	result = rpccli_samr_CreateUser2(pipe_hnd, mem_ctx,
					 &domain_pol,
					 &lsa_acct_name,
					 acb_info,
					 acct_flags,
					 &user_pol,
					 &access_granted,
					 &user_rid);

	/* And restore our original timeout. */
	cli_set_timeout(pipe_hnd->cli, orig_timeout);

	if (!NT_STATUS_IS_OK(result)) {
		d_printf("net rpc trustdom add: create user %s failed %s\n",
			acct_name, nt_errstr(result));
		goto done;
	}

	{
		NTTIME notime;
		struct samr_LogonHours hours;
		struct lsa_BinaryString parameters;
		const int units_per_week = 168;
		uchar pwbuf[516];

		encode_pw_buffer(pwbuf, argv[1], STR_UNICODE);

		ZERO_STRUCT(notime);
		ZERO_STRUCT(hours);
		ZERO_STRUCT(parameters);

		hours.bits = talloc_array(mem_ctx, uint8_t, units_per_week);
		if (!hours.bits) {
			result = NT_STATUS_NO_MEMORY;
			goto done;
		}
		hours.units_per_week = units_per_week;
		memset(hours.bits, 0xFF, units_per_week);

		init_samr_user_info23(&info.info23,
				      notime, notime, notime,
				      notime, notime, notime,
				      NULL, NULL, NULL, NULL, NULL,
				      NULL, NULL, NULL, NULL, &parameters,
				      0, 0, ACB_DOMTRUST, SAMR_FIELD_ACCT_FLAGS,
				      hours,
				      0, 0, 0, 0, 0, 0, 0,
				      pwbuf, 24);

		SamOEMhashBlob(info.info23.password.data, 516,
			       &cli->user_session_key);

		result = rpccli_samr_SetUserInfo2(pipe_hnd, mem_ctx,
						  &user_pol,
						  23,
						  &info);

		if (!NT_STATUS_IS_OK(result)) {
			DEBUG(0,("Could not set trust account password: %s\n",
				 nt_errstr(result)));
			goto done;
		}
	}

 done:
	SAFE_FREE(acct_name);
	return result;
}

/**
 * Create interdomain trust account for a remote domain.
 *
 * @param argc Standard argc.
 * @param argv Standard argv without initial components.
 *
 * @return Integer status (0 means success)
 **/

static int rpc_trustdom_add(int argc, const char **argv)
{
	if (argc > 0) {
		return run_rpc_command(NULL, PI_SAMR, 0, rpc_trustdom_add_internals,
		                       argc, argv);
	} else {
		d_printf("Usage: net rpc trustdom add <domain_name> <trust password>\n");
		return -1;
	}
}


/**
 * Remove interdomain trust account from the RPC server.
 * All parameters (except for argc and argv) are passed by run_rpc_command
 * function.
 *
 * @param domain_sid The domain sid acquired from the server.
 * @param cli A cli_state connected to the server.
 * @param mem_ctx Talloc context, destroyed on completion of the function.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return normal NTSTATUS return code.
 */

static NTSTATUS rpc_trustdom_del_internals(const DOM_SID *domain_sid, 
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx, 
					int argc,
					const char **argv)
{
	POLICY_HND connect_pol, domain_pol, user_pol;
	NTSTATUS result = NT_STATUS_UNSUCCESSFUL;
	char *acct_name;
	DOM_SID trust_acct_sid;
	struct samr_Ids user_rids, name_types;
	struct lsa_String lsa_acct_name;

	if (argc != 1) {
		d_printf("Usage: net rpc trustdom del <domain_name>\n");
		return NT_STATUS_INVALID_PARAMETER;
	}

	/* 
	 * Make valid trusting domain account (ie. uppercased and with '$' appended)
	 */
	acct_name = talloc_asprintf(mem_ctx, "%s$", argv[0]);

	if (acct_name == NULL)
		return NT_STATUS_NO_MEMORY;

	strupper_m(acct_name);

	/* Get samr policy handle */
	result = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
				      pipe_hnd->cli->desthost,
				      MAXIMUM_ALLOWED_ACCESS,
				      &connect_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	/* Get domain policy handle */
	result = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					&connect_pol,
					MAXIMUM_ALLOWED_ACCESS,
					CONST_DISCARD(struct dom_sid2 *, domain_sid),
					&domain_pol);
	if (!NT_STATUS_IS_OK(result)) {
		goto done;
	}

	init_lsa_String(&lsa_acct_name, acct_name);

	result = rpccli_samr_LookupNames(pipe_hnd, mem_ctx,
					 &domain_pol,
					 1,
					 &lsa_acct_name,
					 &user_rids,
					 &name_types);

	if (!NT_STATUS_IS_OK(result)) {
		d_printf("net rpc trustdom del: LookupNames on user %s failed %s\n",
			acct_name, nt_errstr(result) );
		goto done;
	}

	result = rpccli_samr_OpenUser(pipe_hnd, mem_ctx,
				      &domain_pol,
				      MAXIMUM_ALLOWED_ACCESS,
				      user_rids.ids[0],
				      &user_pol);

	if (!NT_STATUS_IS_OK(result)) {
		d_printf("net rpc trustdom del: OpenUser on user %s failed %s\n",
			acct_name, nt_errstr(result) );
		goto done;
	}

	/* append the rid to the domain sid */
	sid_copy(&trust_acct_sid, domain_sid);
	if (!sid_append_rid(&trust_acct_sid, user_rids.ids[0])) {
		goto done;
	}

	/* remove the sid */

	result = rpccli_samr_RemoveMemberFromForeignDomain(pipe_hnd, mem_ctx,
							   &user_pol,
							   &trust_acct_sid);
	if (!NT_STATUS_IS_OK(result)) {
		d_printf("net rpc trustdom del: RemoveMemberFromForeignDomain on user %s failed %s\n",
			acct_name, nt_errstr(result) );
		goto done;
	}

	/* Delete user */

	result = rpccli_samr_DeleteUser(pipe_hnd, mem_ctx,
					&user_pol);

	if (!NT_STATUS_IS_OK(result)) {
		d_printf("net rpc trustdom del: DeleteUser on user %s failed %s\n",
			acct_name, nt_errstr(result) );
		goto done;
	}

	if (!NT_STATUS_IS_OK(result)) {
		d_printf("Could not set trust account password: %s\n",
		   nt_errstr(result));
		goto done;
	}

 done:
	return result;
}

/**
 * Delete interdomain trust account for a remote domain.
 *
 * @param argc Standard argc.
 * @param argv Standard argv without initial components.
 *
 * @return Integer status (0 means success).
 **/

static int rpc_trustdom_del(int argc, const char **argv)
{
	if (argc > 0) {
		return run_rpc_command(NULL, PI_SAMR, 0, rpc_trustdom_del_internals,
		                       argc, argv);
	} else {
		d_printf("Usage: net rpc trustdom del <domain>\n");
		return -1;
	}
}

static NTSTATUS rpc_trustdom_get_pdc(struct cli_state *cli,
				     TALLOC_CTX *mem_ctx,
				     const char *domain_name)
{
	char *dc_name = NULL;
	const char *buffer = NULL;
	struct rpc_pipe_client *netr;
	NTSTATUS status;

	/* Use NetServerEnum2 */

	if (cli_get_pdc_name(cli, domain_name, &dc_name)) {
		SAFE_FREE(dc_name);
		return NT_STATUS_OK;
	}

	DEBUG(1,("NetServerEnum2 error: Couldn't find primary domain controller\
		 for domain %s\n", domain_name));

	/* Try netr_GetDcName */

	netr = cli_rpc_pipe_open_noauth(cli, PI_NETLOGON, &status);
	if (!netr) {
		return status;
	}

	status = rpccli_netr_GetDcName(netr, mem_ctx,
				       cli->desthost,
				       domain_name,
				       &buffer,
				       NULL);
	cli_rpc_pipe_close(netr);

	if (NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(1,("netr_GetDcName error: Couldn't find primary domain controller\
		 for domain %s\n", domain_name));

	return status;
}

/**
 * Establish trust relationship to a trusting domain.
 * Interdomain account must already be created on remote PDC.
 *
 * @param argc Standard argc.
 * @param argv Standard argv without initial components.
 *
 * @return Integer status (0 means success).
 **/

static int rpc_trustdom_establish(int argc, const char **argv)
{
	struct cli_state *cli = NULL;
	struct sockaddr_storage server_ss;
	struct rpc_pipe_client *pipe_hnd = NULL;
	POLICY_HND connect_hnd;
	TALLOC_CTX *mem_ctx;
	NTSTATUS nt_status;
	DOM_SID *domain_sid;

	char* domain_name;
	char* acct_name;
	fstring pdc_name;
	union lsa_PolicyInformation *info = NULL;

	/*
	 * Connect to \\server\ipc$ as 'our domain' account with password
	 */

	if (argc != 1) {
		d_printf("Usage: net rpc trustdom establish <domain_name>\n");
		return -1;
	}

	domain_name = smb_xstrdup(argv[0]);
	strupper_m(domain_name);

	/* account name used at first is our domain's name with '$' */
	asprintf(&acct_name, "%s$", lp_workgroup());
	strupper_m(acct_name);

	/*
	 * opt_workgroup will be used by connection functions further,
	 * hence it should be set to remote domain name instead of ours
	 */
	if (opt_workgroup) {
		opt_workgroup = smb_xstrdup(domain_name);
	};

	opt_user_name = acct_name;

	/* find the domain controller */
	if (!net_find_pdc(&server_ss, pdc_name, domain_name)) {
		DEBUG(0, ("Couldn't find domain controller for domain %s\n", domain_name));
		return -1;
	}

	/* connect to ipc$ as username/password */
	nt_status = connect_to_ipc(&cli, &server_ss, pdc_name);
	if (!NT_STATUS_EQUAL(nt_status, NT_STATUS_NOLOGON_INTERDOMAIN_TRUST_ACCOUNT)) {

		/* Is it trusting domain account for sure ? */
		DEBUG(0, ("Couldn't verify trusting domain account. Error was %s\n",
			nt_errstr(nt_status)));
		return -1;
	}

	/* store who we connected to */

	saf_store( domain_name, pdc_name );

	/*
	 * Connect to \\server\ipc$ again (this time anonymously)
	 */

	nt_status = connect_to_ipc_anonymous(&cli, &server_ss, (char*)pdc_name);

	if (NT_STATUS_IS_ERR(nt_status)) {
		DEBUG(0, ("Couldn't connect to domain %s controller. Error was %s.\n",
			domain_name, nt_errstr(nt_status)));
		return -1;
	}

	if (!(mem_ctx = talloc_init("establishing trust relationship to "
				    "domain %s", domain_name))) {
		DEBUG(0, ("talloc_init() failed\n"));
		cli_shutdown(cli);
		return -1;
	}

	/* Make sure we're talking to a proper server */

	nt_status = rpc_trustdom_get_pdc(cli, mem_ctx, domain_name);
	if (!NT_STATUS_IS_OK(nt_status)) {
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	}

	/*
	 * Call LsaOpenPolicy and LsaQueryInfo
	 */
	 
	pipe_hnd = cli_rpc_pipe_open_noauth(cli, PI_LSARPC, &nt_status);
	if (!pipe_hnd) {
		DEBUG(0, ("Could not initialise lsa pipe. Error was %s\n", nt_errstr(nt_status) ));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	}

	nt_status = rpccli_lsa_open_policy2(pipe_hnd, mem_ctx, True, SEC_RIGHTS_QUERY_VALUE,
	                                 &connect_hnd);
	if (NT_STATUS_IS_ERR(nt_status)) {
		DEBUG(0, ("Couldn't open policy handle. Error was %s\n",
			nt_errstr(nt_status)));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	}

	/* Querying info level 5 */

	nt_status = rpccli_lsa_QueryInfoPolicy(pipe_hnd, mem_ctx,
					       &connect_hnd,
					       LSA_POLICY_INFO_ACCOUNT_DOMAIN,
					       &info);
	if (NT_STATUS_IS_ERR(nt_status)) {
		DEBUG(0, ("LSA Query Info failed. Returned error was %s\n",
			nt_errstr(nt_status)));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	}

	domain_sid = info->account_domain.sid;

	/* There should be actually query info level 3 (following nt serv behaviour),
	   but I still don't know if it's _really_ necessary */
			
	/*
	 * Store the password in secrets db
	 */

	if (!pdb_set_trusteddom_pw(domain_name, opt_password, domain_sid)) {
		DEBUG(0, ("Storing password for trusted domain failed.\n"));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	}
	
	/*
	 * Close the pipes and clean up
	 */
	 
	nt_status = rpccli_lsa_Close(pipe_hnd, mem_ctx, &connect_hnd);
	if (NT_STATUS_IS_ERR(nt_status)) {
		DEBUG(0, ("Couldn't close LSA pipe. Error was %s\n",
			nt_errstr(nt_status)));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	}

	cli_shutdown(cli);
	 
	talloc_destroy(mem_ctx);
	 
	d_printf("Trust to domain %s established\n", domain_name);
	return 0;
}

/**
 * Revoke trust relationship to the remote domain.
 *
 * @param argc Standard argc.
 * @param argv Standard argv without initial components.
 *
 * @return Integer status (0 means success).
 **/

static int rpc_trustdom_revoke(int argc, const char **argv)
{
	char* domain_name;
	int rc = -1;

	if (argc < 1) return -1;
	
	/* generate upper cased domain name */
	domain_name = smb_xstrdup(argv[0]);
	strupper_m(domain_name);

	/* delete password of the trust */
	if (!pdb_del_trusteddom_pw(domain_name)) {
		DEBUG(0, ("Failed to revoke relationship to the trusted domain %s\n",
			  domain_name));
		goto done;
	};
	
	rc = 0;
done:
	SAFE_FREE(domain_name);
	return rc;
}

/**
 * Usage for 'net rpc trustdom' command
 *
 * @param argc standard argc
 * @param argv standard argv without inital components
 *
 * @return Integer status returned to shell
 **/
 
static int rpc_trustdom_usage(int argc, const char **argv)
{
	d_printf("  net rpc trustdom add \t\t add trusting domain's account\n");
	d_printf("  net rpc trustdom del \t\t delete trusting domain's account\n");
	d_printf("  net rpc trustdom establish \t establish relationship to trusted domain\n");
	d_printf("  net rpc trustdom revoke \t abandon relationship to trusted domain\n");
	d_printf("  net rpc trustdom list \t show current interdomain trust relationships\n");
	d_printf("  net rpc trustdom vampire \t vampire interdomain trust relationships from remote server\n");
	return -1;
}


static NTSTATUS rpc_query_domain_sid(const DOM_SID *domain_sid, 
					const char *domain_name, 
					struct cli_state *cli,
					struct rpc_pipe_client *pipe_hnd,
					TALLOC_CTX *mem_ctx,
					int argc,
					const char **argv)
{
	fstring str_sid;
	sid_to_fstring(str_sid, domain_sid);
	d_printf("%s\n", str_sid);
	return NT_STATUS_OK;
}

static void print_trusted_domain(DOM_SID *dom_sid, const char *trusted_dom_name)
{
	fstring ascii_sid, padding;
	int pad_len, col_len = 20;

	/* convert sid into ascii string */
	sid_to_fstring(ascii_sid, dom_sid);

	/* calculate padding space for d_printf to look nicer */
	pad_len = col_len - strlen(trusted_dom_name);
	padding[pad_len] = 0;
	do padding[--pad_len] = ' '; while (pad_len);
			
	d_printf("%s%s%s\n", trusted_dom_name, padding, ascii_sid);
}

static NTSTATUS vampire_trusted_domain(struct rpc_pipe_client *pipe_hnd,
				      TALLOC_CTX *mem_ctx, 
				      POLICY_HND *pol, 
				      DOM_SID dom_sid, 
				      const char *trusted_dom_name)
{
	NTSTATUS nt_status;
	union lsa_TrustedDomainInfo *info = NULL;
	char *cleartextpwd = NULL;
	DATA_BLOB data;

	nt_status = rpccli_lsa_QueryTrustedDomainInfoBySid(pipe_hnd, mem_ctx,
							   pol,
							   &dom_sid,
							   LSA_TRUSTED_DOMAIN_INFO_PASSWORD,
							   &info);
	if (NT_STATUS_IS_ERR(nt_status)) {
		DEBUG(0,("Could not query trusted domain info. Error was %s\n",
		nt_errstr(nt_status)));
		goto done;
	}

	data = data_blob(info->password.password->data,
			 info->password.password->length);

	cleartextpwd = decrypt_trustdom_secret(pipe_hnd->cli->pwd.password,
					       &data);

	if (cleartextpwd == NULL) {
		DEBUG(0,("retrieved NULL password\n"));
		nt_status = NT_STATUS_UNSUCCESSFUL;
		goto done;
	}
	
	if (!pdb_set_trusteddom_pw(trusted_dom_name, cleartextpwd, &dom_sid)) {
		DEBUG(0, ("Storing password for trusted domain failed.\n"));
		nt_status = NT_STATUS_UNSUCCESSFUL;
		goto done;
	}

#ifdef DEBUG_PASSWORD
	DEBUG(100,("successfully vampired trusted domain [%s], sid: [%s], "
		   "password: [%s]\n", trusted_dom_name,
		   sid_string_dbg(&dom_sid), cleartextpwd));
#endif

done:
	SAFE_FREE(cleartextpwd);
	data_blob_free(&data);

	return nt_status;
}

static int rpc_trustdom_vampire(int argc, const char **argv)
{
	/* common variables */
	TALLOC_CTX* mem_ctx;
	struct cli_state *cli = NULL;
	struct rpc_pipe_client *pipe_hnd = NULL;
	NTSTATUS nt_status;
	const char *domain_name = NULL;
	DOM_SID *queried_dom_sid;
	POLICY_HND connect_hnd;
	union lsa_PolicyInformation *info = NULL;

	/* trusted domains listing variables */
	unsigned int enum_ctx = 0;
	int i;
	struct lsa_DomainList dom_list;
	fstring pdc_name;

	/*
	 * Listing trusted domains (stored in secrets.tdb, if local)
	 */

	mem_ctx = talloc_init("trust relationships vampire");

	/*
	 * set domain and pdc name to local samba server (default)
	 * or to remote one given in command line
	 */

	if (StrCaseCmp(opt_workgroup, lp_workgroup())) {
		domain_name = opt_workgroup;
		opt_target_workgroup = opt_workgroup;
	} else {
		fstrcpy(pdc_name, global_myname());
		domain_name = talloc_strdup(mem_ctx, lp_workgroup());
		opt_target_workgroup = domain_name;
	};

	/* open \PIPE\lsarpc and open policy handle */
	nt_status = net_make_ipc_connection(NET_FLAGS_PDC, &cli);
	if (!NT_STATUS_IS_OK(nt_status)) {
		DEBUG(0, ("Couldn't connect to domain controller: %s\n",
			  nt_errstr(nt_status)));
		talloc_destroy(mem_ctx);
		return -1;
	};

	pipe_hnd = cli_rpc_pipe_open_noauth(cli, PI_LSARPC, &nt_status);
	if (!pipe_hnd) {
		DEBUG(0, ("Could not initialise lsa pipe. Error was %s\n",
			nt_errstr(nt_status) ));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	};

	nt_status = rpccli_lsa_open_policy2(pipe_hnd, mem_ctx, False, SEC_RIGHTS_QUERY_VALUE,
					&connect_hnd);
	if (NT_STATUS_IS_ERR(nt_status)) {
		DEBUG(0, ("Couldn't open policy handle. Error was %s\n",
 			nt_errstr(nt_status)));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	};

	/* query info level 5 to obtain sid of a domain being queried */
	nt_status = rpccli_lsa_QueryInfoPolicy(pipe_hnd, mem_ctx,
					       &connect_hnd,
					       LSA_POLICY_INFO_ACCOUNT_DOMAIN,
					       &info);

	if (NT_STATUS_IS_ERR(nt_status)) {
		DEBUG(0, ("LSA Query Info failed. Returned error was %s\n",
			nt_errstr(nt_status)));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	}

	queried_dom_sid = info->account_domain.sid;

	/*
	 * Keep calling LsaEnumTrustdom over opened pipe until
	 * the end of enumeration is reached
	 */

	d_printf("Vampire trusted domains:\n\n");

	do {
		nt_status = rpccli_lsa_EnumTrustDom(pipe_hnd, mem_ctx,
						    &connect_hnd,
						    &enum_ctx,
						    &dom_list,
						    (uint32_t)-1);
		if (NT_STATUS_IS_ERR(nt_status)) {
			DEBUG(0, ("Couldn't enumerate trusted domains. Error was %s\n",
				nt_errstr(nt_status)));
			cli_shutdown(cli);
			talloc_destroy(mem_ctx);
			return -1;
		};

		for (i = 0; i < dom_list.count; i++) {

			print_trusted_domain(dom_list.domains[i].sid,
					     dom_list.domains[i].name.string);

			nt_status = vampire_trusted_domain(pipe_hnd, mem_ctx, &connect_hnd, 
							   *dom_list.domains[i].sid,
							   dom_list.domains[i].name.string);
			if (!NT_STATUS_IS_OK(nt_status)) {
				cli_shutdown(cli);
				talloc_destroy(mem_ctx);
				return -1;
			}
		};

		/*
		 * in case of no trusted domains say something rather
		 * than just display blank line
		 */
		if (!dom_list.count) d_printf("none\n");

	} while (NT_STATUS_EQUAL(nt_status, STATUS_MORE_ENTRIES));

	/* close this connection before doing next one */
	nt_status = rpccli_lsa_Close(pipe_hnd, mem_ctx, &connect_hnd);
	if (NT_STATUS_IS_ERR(nt_status)) {
		DEBUG(0, ("Couldn't properly close lsa policy handle. Error was %s\n",
			nt_errstr(nt_status)));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	};

	/* close lsarpc pipe and connection to IPC$ */
	cli_shutdown(cli);

	talloc_destroy(mem_ctx);	 
	return 0;
}

static int rpc_trustdom_list(int argc, const char **argv)
{
	/* common variables */
	TALLOC_CTX* mem_ctx;
	struct cli_state *cli = NULL, *remote_cli = NULL;
	struct rpc_pipe_client *pipe_hnd = NULL;
	NTSTATUS nt_status;
	const char *domain_name = NULL;
	DOM_SID *queried_dom_sid;
	fstring padding;
	int ascii_dom_name_len;
	POLICY_HND connect_hnd;
	union lsa_PolicyInformation *info = NULL;

	/* trusted domains listing variables */
	unsigned int num_domains, enum_ctx = 0;
	int i, pad_len, col_len = 20;
	struct lsa_DomainList dom_list;
	fstring pdc_name;

	/* trusting domains listing variables */
	POLICY_HND domain_hnd;
	struct samr_SamArray *trusts = NULL;

	/*
	 * Listing trusted domains (stored in secrets.tdb, if local)
	 */

	mem_ctx = talloc_init("trust relationships listing");

	/*
	 * set domain and pdc name to local samba server (default)
	 * or to remote one given in command line
	 */
	
	if (StrCaseCmp(opt_workgroup, lp_workgroup())) {
		domain_name = opt_workgroup;
		opt_target_workgroup = opt_workgroup;
	} else {
		fstrcpy(pdc_name, global_myname());
		domain_name = talloc_strdup(mem_ctx, lp_workgroup());
		opt_target_workgroup = domain_name;
	};

	/* open \PIPE\lsarpc and open policy handle */
	nt_status = net_make_ipc_connection(NET_FLAGS_PDC, &cli);
	if (!NT_STATUS_IS_OK(nt_status)) {
		DEBUG(0, ("Couldn't connect to domain controller: %s\n",
			  nt_errstr(nt_status)));
		talloc_destroy(mem_ctx);
		return -1;
	};

	pipe_hnd = cli_rpc_pipe_open_noauth(cli, PI_LSARPC, &nt_status);
	if (!pipe_hnd) {
		DEBUG(0, ("Could not initialise lsa pipe. Error was %s\n",
			nt_errstr(nt_status) ));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	};

	nt_status = rpccli_lsa_open_policy2(pipe_hnd, mem_ctx, False, SEC_RIGHTS_QUERY_VALUE,
					&connect_hnd);
	if (NT_STATUS_IS_ERR(nt_status)) {
		DEBUG(0, ("Couldn't open policy handle. Error was %s\n",
 			nt_errstr(nt_status)));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	};
	
	/* query info level 5 to obtain sid of a domain being queried */
	nt_status = rpccli_lsa_QueryInfoPolicy(pipe_hnd, mem_ctx,
					       &connect_hnd,
					       LSA_POLICY_INFO_ACCOUNT_DOMAIN,
					       &info);

	if (NT_STATUS_IS_ERR(nt_status)) {
		DEBUG(0, ("LSA Query Info failed. Returned error was %s\n",
			nt_errstr(nt_status)));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	}

	queried_dom_sid = info->account_domain.sid;

	/*
	 * Keep calling LsaEnumTrustdom over opened pipe until
	 * the end of enumeration is reached
	 */
	 
	d_printf("Trusted domains list:\n\n");

	do {
		nt_status = rpccli_lsa_EnumTrustDom(pipe_hnd, mem_ctx,
						    &connect_hnd,
						    &enum_ctx,
						    &dom_list,
						    (uint32_t)-1);
		if (NT_STATUS_IS_ERR(nt_status)) {
			DEBUG(0, ("Couldn't enumerate trusted domains. Error was %s\n",
				nt_errstr(nt_status)));
			cli_shutdown(cli);
			talloc_destroy(mem_ctx);
			return -1;
		};

		for (i = 0; i < dom_list.count; i++) {
			print_trusted_domain(dom_list.domains[i].sid,
					     dom_list.domains[i].name.string);
		};

		/*
		 * in case of no trusted domains say something rather
		 * than just display blank line
		 */
		if (!dom_list.count) d_printf("none\n");

	} while (NT_STATUS_EQUAL(nt_status, STATUS_MORE_ENTRIES));

	/* close this connection before doing next one */
	nt_status = rpccli_lsa_Close(pipe_hnd, mem_ctx, &connect_hnd);
	if (NT_STATUS_IS_ERR(nt_status)) {
		DEBUG(0, ("Couldn't properly close lsa policy handle. Error was %s\n",
			nt_errstr(nt_status)));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	};
	
	cli_rpc_pipe_close(pipe_hnd);

	/*
	 * Listing trusting domains (stored in passdb backend, if local)
	 */
	
	d_printf("\nTrusting domains list:\n\n");

	/*
	 * Open \PIPE\samr and get needed policy handles
	 */
	pipe_hnd = cli_rpc_pipe_open_noauth(cli, PI_SAMR, &nt_status);
	if (!pipe_hnd) {
		DEBUG(0, ("Could not initialise samr pipe. Error was %s\n", nt_errstr(nt_status)));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	};

	/* SamrConnect2 */
	nt_status = rpccli_samr_Connect2(pipe_hnd, mem_ctx,
					 pipe_hnd->cli->desthost,
					 SA_RIGHT_SAM_OPEN_DOMAIN,
					 &connect_hnd);
	if (!NT_STATUS_IS_OK(nt_status)) {
		DEBUG(0, ("Couldn't open SAMR policy handle. Error was %s\n",
			nt_errstr(nt_status)));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	};

	/* SamrOpenDomain - we have to open domain policy handle in order to be
	   able to enumerate accounts*/
	nt_status = rpccli_samr_OpenDomain(pipe_hnd, mem_ctx,
					   &connect_hnd,
					   SA_RIGHT_DOMAIN_ENUM_ACCOUNTS,
					   queried_dom_sid,
					   &domain_hnd);
	if (!NT_STATUS_IS_OK(nt_status)) {
		DEBUG(0, ("Couldn't open domain object. Error was %s\n",
			nt_errstr(nt_status)));
		cli_shutdown(cli);
		talloc_destroy(mem_ctx);
		return -1;
	};
	
	/*
	 * perform actual enumeration
	 */
	 
	enum_ctx = 0;	/* reset enumeration context from last enumeration */
	do {

		nt_status = rpccli_samr_EnumDomainUsers(pipe_hnd, mem_ctx,
							&domain_hnd,
							&enum_ctx,
							ACB_DOMTRUST,
							&trusts,
							0xffff,
							&num_domains);
		if (NT_STATUS_IS_ERR(nt_status)) {
			DEBUG(0, ("Couldn't enumerate accounts. Error was: %s\n",
				nt_errstr(nt_status)));
			cli_shutdown(cli);
			talloc_destroy(mem_ctx);
			return -1;
		};

		for (i = 0; i < num_domains; i++) {

			char *str = CONST_DISCARD(char *, trusts->entries[i].name.string);

			/*
			 * get each single domain's sid (do we _really_ need this ?):
			 *  1) connect to domain's pdc
			 *  2) query the pdc for domain's sid
			 */

			/* get rid of '$' tail */
			ascii_dom_name_len = strlen(str);
			if (ascii_dom_name_len && ascii_dom_name_len < FSTRING_LEN)
				str[ascii_dom_name_len - 1] = '\0';

			/* calculate padding space for d_printf to look nicer */
			pad_len = col_len - strlen(str);
			padding[pad_len] = 0;
			do padding[--pad_len] = ' '; while (pad_len);

			/* set opt_* variables to remote domain */
			strupper_m(str);
			opt_workgroup = talloc_strdup(mem_ctx, str);
			opt_target_workgroup = opt_workgroup;

			d_printf("%s%s", str, padding);

			/* connect to remote domain controller */
			nt_status = net_make_ipc_connection(
					NET_FLAGS_PDC | NET_FLAGS_ANONYMOUS,
					&remote_cli);
			if (NT_STATUS_IS_OK(nt_status)) {
				/* query for domain's sid */
				if (run_rpc_command(remote_cli, PI_LSARPC, 0, rpc_query_domain_sid, argc, argv))
					d_fprintf(stderr, "couldn't get domain's sid\n");

				cli_shutdown(remote_cli);
			
			} else {
				d_fprintf(stderr, "domain controller is not "
					  "responding: %s\n",
					  nt_errstr(nt_status));
			};
		};
		
		if (!num_domains) d_printf("none\n");
		
	} while (NT_STATUS_EQUAL(nt_status, STATUS_MORE_ENTRIES));

	/* close opened samr and domain policy handles */
	nt_status = rpccli_samr_Close(pipe_hnd, mem_ctx, &domain_hnd);
	if (!NT_STATUS_IS_OK(nt_status)) {
		DEBUG(0, ("Couldn't properly close domain policy handle for domain %s\n", domain_name));
	};
	
	nt_status = rpccli_samr_Close(pipe_hnd, mem_ctx, &connect_hnd);
	if (!NT_STATUS_IS_OK(nt_status)) {
		DEBUG(0, ("Couldn't properly close samr policy handle for domain %s\n", domain_name));
	};
	
	/* close samr pipe and connection to IPC$ */
	cli_shutdown(cli);

	talloc_destroy(mem_ctx);	 
	return 0;
}

/**
 * Entrypoint for 'net rpc trustdom' code.
 *
 * @param argc Standard argc.
 * @param argv Standard argv without initial components.
 *
 * @return Integer status (0 means success).
 */

static int rpc_trustdom(int argc, const char **argv)
{
	struct functable func[] = {
		{"add", rpc_trustdom_add},
		{"del", rpc_trustdom_del},
		{"establish", rpc_trustdom_establish},
		{"revoke", rpc_trustdom_revoke},
		{"help", rpc_trustdom_usage},
		{"list", rpc_trustdom_list},
		{"vampire", rpc_trustdom_vampire},
		{NULL, NULL}
	};

	if (argc == 0) {
		rpc_trustdom_usage(argc, argv);
		return -1;
	}

	return (net_run_function(argc, argv, func, rpc_trustdom_usage));
}

/**
 * Check if a server will take rpc commands
 * @param flags	Type of server to connect to (PDC, DMB, localhost)
 *		if the host is not explicitly specified
 * @return  bool (true means rpc supported)
 */
bool net_rpc_check(unsigned flags)
{
	struct cli_state *cli;
	bool ret = False;
	struct sockaddr_storage server_ss;
	char *server_name = NULL;
	NTSTATUS status;

	/* flags (i.e. server type) may depend on command */
	if (!net_find_server(NULL, flags, &server_ss, &server_name))
		return False;

	if ((cli = cli_initialise()) == NULL) {
		return False;
	}

	status = cli_connect(cli, server_name, &server_ss);
	if (!NT_STATUS_IS_OK(status))
		goto done;
	if (!attempt_netbios_session_request(&cli, global_myname(),
					     server_name, &server_ss))
		goto done;
	if (!cli_negprot(cli))
		goto done;
	if (cli->protocol < PROTOCOL_NT1)
		goto done;

	ret = True;
 done:
	cli_shutdown(cli);
	return ret;
}

/* dump sam database via samsync rpc calls */
static int rpc_samdump(int argc, const char **argv) {
		return run_rpc_command(NULL, PI_NETLOGON, NET_FLAGS_ANONYMOUS, rpc_samdump_internals,
			       argc, argv);
}

/* syncronise sam database via samsync rpc calls */
static int rpc_vampire(int argc, const char **argv) {
	return run_rpc_command(NULL, PI_NETLOGON, NET_FLAGS_ANONYMOUS, rpc_vampire_internals,
			       argc, argv);
}

/** 
 * Migrate everything from a print-server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 *
 * The order is important !
 * To successfully add drivers the print-queues have to exist !
 * Applying ACLs should be the last step, because you're easily locked out.
 *
 **/
static int rpc_printer_migrate_all(int argc, const char **argv)
{
	int ret;

	if (!opt_host) {
		printf("no server to migrate\n");
		return -1;
	}

	ret = run_rpc_command(NULL, PI_SPOOLSS, 0, rpc_printer_migrate_printers_internals, argc, argv);
	if (ret)
		return ret;

	ret = run_rpc_command(NULL, PI_SPOOLSS, 0, rpc_printer_migrate_drivers_internals, argc, argv);
	if (ret)
		return ret;

	ret = run_rpc_command(NULL, PI_SPOOLSS, 0, rpc_printer_migrate_forms_internals, argc, argv);
	if (ret)
		return ret;

	ret = run_rpc_command(NULL, PI_SPOOLSS, 0, rpc_printer_migrate_settings_internals, argc, argv);
	if (ret)
		return ret;

	return run_rpc_command(NULL, PI_SPOOLSS, 0, rpc_printer_migrate_security_internals, argc, argv);

}

/** 
 * Migrate print-drivers from a print-server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_printer_migrate_drivers(int argc, const char **argv)
{
	if (!opt_host) {
		printf("no server to migrate\n");
		return -1;
	}

	return run_rpc_command(NULL, PI_SPOOLSS, 0, 
			       rpc_printer_migrate_drivers_internals,
			       argc, argv);
}

/** 
 * Migrate print-forms from a print-server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_printer_migrate_forms(int argc, const char **argv)
{
	if (!opt_host) {
		printf("no server to migrate\n");
		return -1;
	}

	return run_rpc_command(NULL, PI_SPOOLSS, 0, 
			       rpc_printer_migrate_forms_internals,
			       argc, argv);
}

/** 
 * Migrate printers from a print-server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_printer_migrate_printers(int argc, const char **argv)
{
	if (!opt_host) {
		printf("no server to migrate\n");
		return -1;
	}

	return run_rpc_command(NULL, PI_SPOOLSS, 0, 
			       rpc_printer_migrate_printers_internals,
			       argc, argv);
}

/** 
 * Migrate printer-ACLs from a print-server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_printer_migrate_security(int argc, const char **argv)
{
	if (!opt_host) {
		printf("no server to migrate\n");
		return -1;
	}

	return run_rpc_command(NULL, PI_SPOOLSS, 0, 
			       rpc_printer_migrate_security_internals,
			       argc, argv);
}

/** 
 * Migrate printer-settings from a print-server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_printer_migrate_settings(int argc, const char **argv)
{
	if (!opt_host) {
		printf("no server to migrate\n");
		return -1;
	}

	return run_rpc_command(NULL, PI_SPOOLSS, 0, 
			       rpc_printer_migrate_settings_internals,
			       argc, argv);
}

/** 
 * 'net rpc printer' entrypoint.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/

int rpc_printer_migrate(int argc, const char **argv) 
{

	/* ouch: when addriver and setdriver are called from within
	   rpc_printer_migrate_drivers_internals, the printer-queue already
	   *has* to exist */

	struct functable func[] = {
		{"all", 	rpc_printer_migrate_all},
		{"drivers", 	rpc_printer_migrate_drivers},
		{"forms", 	rpc_printer_migrate_forms},
		{"help", 	rpc_printer_usage},
		{"printers", 	rpc_printer_migrate_printers},
		{"security", 	rpc_printer_migrate_security},
		{"settings", 	rpc_printer_migrate_settings},
		{NULL, NULL}
	};

	return net_run_function(argc, argv, func, rpc_printer_usage);
}


/** 
 * List printers on a remote RPC server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_printer_list(int argc, const char **argv)
{

	return run_rpc_command(NULL, PI_SPOOLSS, 0, 
			       rpc_printer_list_internals,
			       argc, argv);
}

/** 
 * List printer-drivers on a remote RPC server.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv.  Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_printer_driver_list(int argc, const char **argv)
{

	return run_rpc_command(NULL, PI_SPOOLSS, 0, 
			       rpc_printer_driver_list_internals,
			       argc, argv);
}

/** 
 * Publish printer in ADS via MSRPC.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_printer_publish_publish(int argc, const char **argv)
{

	return run_rpc_command(NULL, PI_SPOOLSS, 0, 
			       rpc_printer_publish_publish_internals,
			       argc, argv);
}

/** 
 * Update printer in ADS via MSRPC.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_printer_publish_update(int argc, const char **argv)
{

	return run_rpc_command(NULL, PI_SPOOLSS, 0, 
			       rpc_printer_publish_update_internals,
			       argc, argv);
}

/** 
 * UnPublish printer in ADS via MSRPC
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv.  Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_printer_publish_unpublish(int argc, const char **argv)
{

	return run_rpc_command(NULL, PI_SPOOLSS, 0, 
			       rpc_printer_publish_unpublish_internals,
			       argc, argv);
}

/** 
 * List published printers via MSRPC.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_printer_publish_list(int argc, const char **argv)
{

	return run_rpc_command(NULL, PI_SPOOLSS, 0, 
			       rpc_printer_publish_list_internals,
			       argc, argv);
}


/** 
 * Publish printer in ADS.
 *
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 *
 * @return A shell status integer (0 for success).
 **/
static int rpc_printer_publish(int argc, const char **argv)
{

	struct functable func[] = {
		{"publish", 	rpc_printer_publish_publish},
		{"update", 	rpc_printer_publish_update},
		{"unpublish", 	rpc_printer_publish_unpublish},
		{"list", 	rpc_printer_publish_list},
		{"help", 	rpc_printer_usage},
		{NULL, NULL}
	};

	if (argc == 0)
		return run_rpc_command(NULL, PI_SPOOLSS, 0, 
			       rpc_printer_publish_list_internals,
			       argc, argv);

	return net_run_function(argc, argv, func, rpc_printer_usage);

}


/** 
 * Display rpc printer help page.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/
int rpc_printer_usage(int argc, const char **argv)
{
        return net_help_printer(argc, argv);
}

/** 
 * 'net rpc printer' entrypoint.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/
int net_rpc_printer(int argc, const char **argv) 
{
	struct functable func[] = {
		{"list", rpc_printer_list},
		{"migrate", rpc_printer_migrate},
		{"driver", rpc_printer_driver_list},
		{"publish", rpc_printer_publish},
		{NULL, NULL}
	};

	if (argc == 0)
		return run_rpc_command(NULL, PI_SPOOLSS, 0, 
			       rpc_printer_list_internals,
			       argc, argv);

	return net_run_function(argc, argv, func, rpc_printer_usage);
}

/****************************************************************************/


/** 
 * Basic usage function for 'net rpc'.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/

int net_rpc_usage(int argc, const char **argv) 
{
	d_printf("  net rpc info \t\t\tshow basic info about a domain \n");
	d_printf("  net rpc join \t\t\tto join a domain \n");
	d_printf("  net rpc oldjoin \t\tto join a domain created in server manager\n");
	d_printf("  net rpc testjoin \t\ttests that a join is valid\n");
	d_printf("  net rpc user \t\t\tto add, delete and list users\n");
	d_printf("  net rpc password <username> [<password>] -Uadmin_username%%admin_pass\n");
	d_printf("  net rpc group \t\tto list groups\n");
	d_printf("  net rpc share \t\tto add, delete, list and migrate shares\n");
	d_printf("  net rpc printer \t\tto list and migrate printers\n");
	d_printf("  net rpc file \t\t\tto list open files\n");
	d_printf("  net rpc changetrustpw \tto change the trust account password\n");
	d_printf("  net rpc getsid \t\tfetch the domain sid into the local secrets.tdb\n");
	d_printf("  net rpc vampire \t\tsyncronise an NT PDC's users and groups into the local passdb\n");
	d_printf("  net rpc samdump \t\tdisplay an NT PDC's users, groups and other data\n");
	d_printf("  net rpc trustdom \t\tto create trusting domain's account or establish trust\n");
	d_printf("  net rpc abortshutdown \tto abort the shutdown of a remote server\n");
	d_printf("  net rpc shutdown \t\tto shutdown a remote server\n");
	d_printf("  net rpc rights\t\tto manage privileges assigned to SIDs\n");
	d_printf("  net rpc registry\t\tto manage registry hives\n");
	d_printf("  net rpc service\t\tto start, stop and query services\n");
	d_printf("  net rpc audit\t\t\tto modify global auditing settings\n");
	d_printf("  net rpc shell\t\t\tto open an interactive shell for remote server/account management\n");
	d_printf("\n");
	d_printf("'net rpc shutdown' also accepts the following miscellaneous options:\n"); /* misc options */
	d_printf("\t-r or --reboot\trequest remote server reboot on shutdown\n");
	d_printf("\t-f or --force\trequest the remote server force its shutdown\n");
	d_printf("\t-t or --timeout=<timeout>\tnumber of seconds before shutdown\n");
	d_printf("\t-C or --comment=<message>\ttext message to display on impending shutdown\n");
	return -1;
}


/**
 * Help function for 'net rpc'.  Calls command specific help if requested
 * or displays usage of net rpc.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/

int net_rpc_help(int argc, const char **argv)
{
	struct functable func[] = {
		{"join", rpc_join_usage},
		{"user", rpc_user_usage},
		{"group", rpc_group_usage},
		{"share", rpc_share_usage},
		/*{"changetrustpw", rpc_changetrustpw_usage}, */
		{"trustdom", rpc_trustdom_usage},
		/*{"abortshutdown", rpc_shutdown_abort_usage},*/
		/*{"shutdown", rpc_shutdown_usage}, */
		{"vampire", rpc_vampire_usage},
		{NULL, NULL}
	};

	if (argc == 0) {
		net_rpc_usage(argc, argv);
		return -1;
	}

	return (net_run_function(argc, argv, func, rpc_user_usage));
}

/** 
 * 'net rpc' entrypoint.
 * @param argc  Standard main() style argc.
 * @param argv  Standard main() style argv. Initial components are already
 *              stripped.
 **/

int net_rpc(int argc, const char **argv)
{
	struct functable func[] = {
		{"audit", net_rpc_audit},
		{"info", net_rpc_info},
		{"join", net_rpc_join},
		{"oldjoin", net_rpc_oldjoin},
		{"testjoin", net_rpc_testjoin},
		{"user", net_rpc_user},
		{"password", rpc_user_password},
		{"group", net_rpc_group},
		{"share", net_rpc_share},
		{"file", net_rpc_file},
		{"printer", net_rpc_printer},
		{"changetrustpw", net_rpc_changetrustpw},
		{"trustdom", rpc_trustdom},
		{"abortshutdown", rpc_shutdown_abort},
		{"shutdown", rpc_shutdown},
		{"samdump", rpc_samdump},
		{"vampire", rpc_vampire},
		{"getsid", net_rpc_getsid},
		{"rights", net_rpc_rights},
		{"service", net_rpc_service},
		{"registry", net_rpc_registry},
		{"shell", net_rpc_shell},
		{"help", net_rpc_help},
		{NULL, NULL}
	};
	return net_run_function(argc, argv, func, net_rpc_usage);
}
