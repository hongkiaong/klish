#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <poll.h>
#include <sys/wait.h>

#include <faux/str.h>
#include <faux/async.h>
#include <faux/msg.h>
#include <faux/eloop.h>
#include <klish/ksession.h>
#include <klish/ksession_parse.h>
#include <klish/ktp.h>
#include <klish/ktp_session.h>


typedef enum {
	KTPD_SESSION_STATE_DISCONNECTED = 'd',
	KTPD_SESSION_STATE_UNAUTHORIZED = 'a',
	KTPD_SESSION_STATE_IDLE = 'i',
	KTPD_SESSION_STATE_WAIT_FOR_PROCESS = 'p',
} ktpd_session_state_e;


struct ktpd_session_s {
	ksession_t *session;
	ktpd_session_state_e state;
	uid_t uid;
	gid_t gid;
	char *user;
	faux_async_t *async;
	faux_hdr_t *hdr; // Engine will receive header and then msg
	faux_eloop_t *eloop; // External link, dont's free()
	kexec_t *exec;
	bool_t exit;
};


// Static declarations
static bool_t ktpd_session_read_cb(faux_async_t *async,
	faux_buf_t *buf, size_t len, void *user_data);
static bool_t wait_for_actions_ev(faux_eloop_t *eloop, faux_eloop_type_e type,
	void *associated_data, void *user_data);
bool_t client_ev(faux_eloop_t *eloop, faux_eloop_type_e type,
	void *associated_data, void *user_data);
static bool_t ktpd_session_exec(ktpd_session_t *ktpd, const char *line,
	int *retcode, faux_error_t *error, bool_t dry_run);
static bool_t action_stdout_ev(faux_eloop_t *eloop, faux_eloop_type_e type,
	void *associated_data, void *user_data);
static bool_t action_stderr_ev(faux_eloop_t *eloop, faux_eloop_type_e type,
	void *associated_data, void *user_data);


ktpd_session_t *ktpd_session_new(int sock, kscheme_t *scheme,
	const char *start_entry, faux_eloop_t *eloop)
{
	ktpd_session_t *ktpd = NULL;

	if (sock < 0)
		return NULL;
	if (!eloop)
		return NULL;

	ktpd = faux_zmalloc(sizeof(*ktpd));
	assert(ktpd);
	if (!ktpd)
		return NULL;

	// Init
	ktpd->state = KTPD_SESSION_STATE_IDLE;
	ktpd->eloop = eloop;
	ktpd->session = ksession_new(scheme, start_entry);
	assert(ktpd->session);
	ktpd->exec = NULL;
	// Exit flag. It differs from ksession done flag because KTPD session
	// can't exit immediately. It must finish current command processing
	// before really stop the event loop. Note: User defined plugin
	// function must use ksession done flag. This exit flag is internal
	// feature of KTPD session.
	ktpd->exit = BOOL_FALSE;

	// Async object
	ktpd->async = faux_async_new(sock);
	assert(ktpd->async);
	// Receive message header first
	faux_async_set_read_limits(ktpd->async,
		sizeof(faux_hdr_t), sizeof(faux_hdr_t));
	faux_async_set_read_cb(ktpd->async, ktpd_session_read_cb, ktpd);
	ktpd->hdr = NULL;
	faux_async_set_stall_cb(ktpd->async, ktp_stall_cb, ktpd->eloop);

	// Eloop callbacks
	faux_eloop_add_fd(ktpd->eloop, ktpd_session_fd(ktpd), POLLIN,
		client_ev, ktpd);
	faux_eloop_add_signal(ktpd->eloop, SIGCHLD, wait_for_actions_ev, ktpd);

	return ktpd;
}


void ktpd_session_free(ktpd_session_t *ktpd)
{
	if (!ktpd)
		return;

	kexec_free(ktpd->exec);
	ksession_free(ktpd->session);
	faux_free(ktpd->hdr);
	close(ktpd_session_fd(ktpd));
	faux_async_free(ktpd->async);
	faux_free(ktpd);
}


static bool_t ktpd_session_process_cmd(ktpd_session_t *ktpd, faux_msg_t *msg)
{
	char *line = NULL;
	int retcode = -1;
	ktp_cmd_e cmd = KTP_CMD_ACK;
	faux_error_t *error = NULL;
	bool_t rc = BOOL_FALSE;
	bool_t dry_run = BOOL_FALSE;
	uint32_t status = KTP_STATUS_NONE;
	bool_t ret = BOOL_TRUE;

	assert(ktpd);
	assert(msg);

	// Get line from message
	if (!(line = faux_msg_get_str_param_by_type(msg, KTP_PARAM_LINE))) {
		ktp_send_error(ktpd->async, cmd, "The line is not specified");
		return BOOL_FALSE;
	}

	// Get dry-run flag from message
	if (KTP_STATUS_IS_DRY_RUN(faux_msg_get_status(msg)))
		dry_run = BOOL_TRUE;

	error = faux_error_new();

	ktpd->exec = NULL;
	rc = ktpd_session_exec(ktpd, line, &retcode, error, dry_run);
	faux_str_free(line);

	// Command is scheduled. Eloop will wait for ACTION completion.
	// So inform client about it and about command features like
	// interactive/non-interactive.
	if (ktpd->exec) {
		faux_msg_t *ack = NULL;
		ktp_status_e status = KTP_STATUS_INCOMPLETED;
		ack = ktp_msg_preform(cmd, status);
		faux_msg_send_async(ack, ktpd->async);
		faux_msg_free(ack);
		faux_error_free(error);
		return BOOL_TRUE; // Continue and wait for ACTION
	}

	// Here we don't need to wait for the action. We have retcode already.
	if (ksession_done(ktpd->session)) {
		ktpd->exit = BOOL_TRUE;
		status |= KTP_STATUS_EXIT;
	}

	if (rc) {
		uint8_t retcode8bit = 0;
		faux_msg_t *ack = ktp_msg_preform(cmd, status);
		retcode8bit = (uint8_t)(retcode & 0xff);
		faux_msg_add_param(ack, KTP_PARAM_RETCODE, &retcode8bit, 1);
		faux_msg_send_async(ack, ktpd->async);
		faux_msg_free(ack);
	} else {
		char *err = faux_error_cstr(error);
		ktp_send_error(ktpd->async, cmd, err);
		faux_str_free(err);
		ret = BOOL_FALSE;
	}

	faux_error_free(error);

	return ret;
}


static bool_t ktpd_session_exec(ktpd_session_t *ktpd, const char *line,
	int *retcode, faux_error_t *error, bool_t dry_run)
{
	kexec_t *exec = NULL;

	assert(ktpd);
	if (!ktpd)
		return BOOL_FALSE;

	// Parsing
	exec = ksession_parse_for_exec(ktpd->session, line, error);
	if (!exec)
		return BOOL_FALSE;

	// Set dry-run flag
	kexec_set_dry_run(exec, dry_run);

	// Session status can be changed while parsing
// NOTE: kexec_t is atomic now
//	if (ksession_done(ktpd->session)) {
//		kexec_free(exec);
//		return BOOL_FALSE; // Because action is not completed
//	}

	// Execute kexec and then wait for completion using global Eloop
	if (!kexec_exec(exec)) {
		kexec_free(exec);
		return BOOL_FALSE; // Something went wrong
	}
	// If kexec contains only non-exec (for example dry-run) ACTIONs then
	// we don't need event loop and can return here.
	if (kexec_retcode(exec, retcode)) {
		kexec_free(exec);
		return BOOL_TRUE;
	}

	// Save kexec pointer to use later
	ktpd->state = KTPD_SESSION_STATE_WAIT_FOR_PROCESS;
	ktpd->exec = exec;

	faux_eloop_add_fd(ktpd->eloop, kexec_stdout(exec), POLLIN,
		action_stdout_ev, ktpd);
	faux_eloop_add_fd(ktpd->eloop, kexec_stderr(exec), POLLIN,
		action_stderr_ev, ktpd);

	return BOOL_TRUE;
}


static bool_t wait_for_actions_ev(faux_eloop_t *eloop, faux_eloop_type_e type,
	void *associated_data, void *user_data)
{
	int wstatus = 0;
	pid_t child_pid = -1;
	ktpd_session_t *ktpd = (ktpd_session_t *)user_data;
	int retcode = -1;
	uint8_t retcode8bit = 0;
	faux_msg_t *ack = NULL;
	ktp_cmd_e cmd = KTP_CMD_ACK;
	uint32_t status = KTP_STATUS_NONE;

	if (!ktpd)
		return BOOL_FALSE;

	// Wait for any child process. Doesn't block.
	while ((child_pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
		if (ktpd->exec)
			kexec_continue_command_execution(ktpd->exec, child_pid,
				wstatus);
	}
	if (!ktpd->exec)
		return BOOL_TRUE;

	// Check if kexec is done now
	if (!kexec_retcode(ktpd->exec, &retcode))
		return BOOL_TRUE; // Continue

	faux_eloop_del_fd(eloop, kexec_stdout(ktpd->exec));
	faux_eloop_del_fd(eloop, kexec_stderr(ktpd->exec));

	kexec_free(ktpd->exec);
	ktpd->exec = NULL;
	ktpd->state = KTPD_SESSION_STATE_IDLE;

	// All kexec_t actions are done so can break the loop if needed.
	if (ksession_done(ktpd->session)) {
		ktpd->exit = BOOL_TRUE;
		status |= KTP_STATUS_EXIT; // Notify client about exiting
	}

	// Send ACK message
	ack = ktp_msg_preform(cmd, status);
	retcode8bit = (uint8_t)(retcode & 0xff);
	faux_msg_add_param(ack, KTP_PARAM_RETCODE, &retcode8bit, 1);
	faux_msg_send_async(ack, ktpd->async);
	faux_msg_free(ack);

	type = type; // Happy compiler
	associated_data = associated_data; // Happy compiler

	if (ktpd->exit)
		return BOOL_FALSE;

	return BOOL_TRUE;
}


static int compl_compare(const void *first, const void *second)
{
	const char *f = (const char *)first;
	const char *s = (const char *)second;

	return strcmp(f, s);
}


static int compl_kcompare(const void *key, const void *list_item)
{
	const char *f = (const char *)key;
	const char *s = (const char *)list_item;

	return strcmp(f, s);
}


static bool_t ktpd_session_process_completion(ktpd_session_t *ktpd, faux_msg_t *msg)
{
	char *line = NULL;
	faux_msg_t *ack = NULL;
	kpargv_t *pargv = NULL;
	ktp_cmd_e cmd = KTP_COMPLETION_ACK;
	uint32_t status = KTP_STATUS_NONE;
	const char *prefix = NULL;
	size_t prefix_len = 0;

	assert(ktpd);
	assert(msg);

	// Get line from message
	if (!(line = faux_msg_get_str_param_by_type(msg, KTP_PARAM_LINE))) {
		ktp_send_error(ktpd->async, cmd, NULL);
		return BOOL_FALSE;
	}

	// Parsing
	pargv = ksession_parse_for_completion(ktpd->session, line);
	faux_str_free(line);
	if (!pargv) {
		ktp_send_error(ktpd->async, cmd, NULL);
		return BOOL_FALSE;
	}
	kpargv_debug(pargv);

	if (ksession_done(ktpd->session)) {
		ktpd->exit = BOOL_TRUE;
		status |= KTP_STATUS_EXIT; // Notify client about exiting
	}

	// Prepare ACK message
	ack = ktp_msg_preform(cmd, status);

	// Last unfinished word. Common prefix for all completions
	prefix = kpargv_last_arg(pargv);
	if (!faux_str_is_empty(prefix)) {
		prefix_len = strlen(prefix);
		faux_msg_add_param(ack, KTP_PARAM_PREFIX, prefix, prefix_len);
	}

	// Fill msg with possible completions
	if (!kpargv_completions_is_empty(pargv)) {
		const kentry_t *candidate = NULL;
		kpargv_completions_node_t *citer = kpargv_completions_iter(pargv);
		faux_list_t *completions = NULL;

		completions = faux_list_new(FAUX_LIST_SORTED, FAUX_LIST_UNIQUE,
			compl_compare, compl_kcompare,
			(void (*)(void *))faux_str_free);
		while ((candidate = kpargv_completions_each(&citer))) {
			const kentry_t *completion = NULL;
			kparg_t *parg = NULL;
			int rc = -1;
			char *out = NULL;
			bool_t res = BOOL_FALSE;
			char *l = NULL; // One line of completion
			const char *str = NULL;

			// Get completion entry from candidate entry
			completion = kentry_nested_by_purpose(candidate,
				KENTRY_PURPOSE_COMPLETION);
			// If candidate entry doesn't contain completion then try
			// to get completion from entry's PTYPE
			if (!completion) {
				const kentry_t *ptype = NULL;
				ptype = kentry_nested_by_purpose(candidate,
					KENTRY_PURPOSE_PTYPE);
				if (!ptype)
					continue;
				completion = kentry_nested_by_purpose(ptype,
					KENTRY_PURPOSE_COMPLETION);
			}
			if (!completion)
				continue;
			parg = kparg_new(candidate, prefix);
			kpargv_set_candidate_parg(pargv, parg);
			res = ksession_exec_locally(ktpd->session, completion,
				pargv, &rc, &out);
			kparg_free(parg);
			if (!res || (rc < 0) || !out)
				continue;

			// Get all completions one by one
			str = out;
			while ((l = faux_str_getline(str, &str))) {
				char *compl_str = l;
				// Compare prefix
				if ((prefix_len > 0) &&
					(faux_str_cmpn(prefix, l, prefix_len) != 0)) {
					faux_str_free(l);
					continue;
				}
				compl_str = l + prefix_len;
				faux_msg_add_param(ack, KTP_PARAM_LINE,
					compl_str, strlen(compl_str));
				faux_list_add(completions, faux_str_dup(compl_str));
				faux_str_free(l);
			}

			faux_str_free(out);
		}
		faux_list_free(completions);
	}

	faux_msg_send_async(ack, ktpd->async);
	faux_msg_free(ack);

	kpargv_free(pargv);

	return BOOL_TRUE;
}


static bool_t ktpd_session_process_help(ktpd_session_t *ktpd, faux_msg_t *msg)
{
	char *line = NULL;
	faux_msg_t *ack = NULL;
//	kpargv_t *pargv = NULL;
	ktp_cmd_e cmd = KTP_HELP_ACK;

	assert(ktpd);
	assert(msg);

	// Get line from message
	if (!(line = faux_msg_get_str_param_by_type(msg, KTP_PARAM_LINE))) {
		ktp_send_error(ktpd->async, cmd, NULL);
		return BOOL_FALSE;
	}

/*	// Parsing
	pargv = ksession_parse_line(ktpd->session, line, KPURPOSE_HELP);
	faux_str_free(line);
	kpargv_free(pargv);
*/
	// Send ACK message
	ack = ktp_msg_preform(cmd, KTP_STATUS_NONE);
	faux_msg_send_async(ack, ktpd->async);
	faux_msg_free(ack);

	return BOOL_TRUE;
}


static bool_t ktpd_session_dispatch(ktpd_session_t *ktpd, faux_msg_t *msg)
{
	uint16_t cmd = 0;

	assert(ktpd);
	if (!ktpd)
		return BOOL_FALSE;
	assert(msg);
	if (!msg)
		return BOOL_FALSE;

	cmd = faux_msg_get_cmd(msg);
	switch (cmd) {
	case KTP_CMD:
		ktpd_session_process_cmd(ktpd, msg);
		break;
	case KTP_COMPLETION:
		ktpd_session_process_completion(ktpd, msg);
		break;
	case KTP_HELP:
		ktpd_session_process_help(ktpd, msg);
		break;
	default:
		syslog(LOG_WARNING, "Unsupported command: 0x%04u\n", cmd);
		break;
	}

	return BOOL_TRUE;
}


/** @brief Low-level function to receive KTP message.
 *
 * Firstly function gets the header of message. Then it checks and parses
 * header and find out the length of whole message. Then it receives the rest
 * of message.
 */
static bool_t ktpd_session_read_cb(faux_async_t *async,
	faux_buf_t *buf, size_t len, void *user_data)
{
	ktpd_session_t *ktpd = (ktpd_session_t *)user_data;
	faux_msg_t *completed_msg = NULL;
	char *data = NULL;

	assert(async);
	assert(buf);
	assert(ktpd);

	// Linearize buffer
	data = malloc(len);
	faux_buf_read(buf, data, len);

	// Receive header
	if (!ktpd->hdr) {
		size_t whole_len = 0;
		size_t msg_wo_hdr = 0;

		ktpd->hdr = (faux_hdr_t *)data;
		// Check for broken header
		if (!ktp_check_header(ktpd->hdr)) {
			faux_free(ktpd->hdr);
			ktpd->hdr = NULL;
			return BOOL_FALSE;
		}

		whole_len = faux_hdr_len(ktpd->hdr);
		// msg_wo_hdr >= 0 because ktp_check_header() validates whole_len
		msg_wo_hdr = whole_len - sizeof(faux_hdr_t);
		// Plan to receive message body
		if (msg_wo_hdr > 0) {
			faux_async_set_read_limits(async,
				msg_wo_hdr, msg_wo_hdr);
			return BOOL_TRUE;
		}
		// Here message is completed (msg body has zero length)
		completed_msg = faux_msg_deserialize_parts(ktpd->hdr, NULL, 0);

	// Receive message body
	} else {
		completed_msg = faux_msg_deserialize_parts(ktpd->hdr, data, len);
		faux_free(data);
	}

	// Plan to receive msg header
	faux_async_set_read_limits(ktpd->async,
		sizeof(faux_hdr_t), sizeof(faux_hdr_t));
	faux_free(ktpd->hdr);
	ktpd->hdr = NULL; // Ready to recv new header

	// Here message is completed
	ktpd_session_dispatch(ktpd, completed_msg);
	faux_msg_free(completed_msg);

	return BOOL_TRUE;
}


bool_t ktpd_session_connected(ktpd_session_t *ktpd)
{
	assert(ktpd);
	if (!ktpd)
		return BOOL_FALSE;
	if (KTPD_SESSION_STATE_DISCONNECTED == ktpd->state)
		return BOOL_FALSE;

	return BOOL_TRUE;
}


int ktpd_session_fd(const ktpd_session_t *ktpd)
{
	assert(ktpd);
	if (!ktpd)
		return BOOL_FALSE;

	return faux_async_fd(ktpd->async);
}


static bool_t action_stdout_ev(faux_eloop_t *eloop, faux_eloop_type_e type,
	void *associated_data, void *user_data)
{
	faux_eloop_info_fd_t *info = (faux_eloop_info_fd_t *)associated_data;
	ktpd_session_t *ktpd = (ktpd_session_t *)user_data;
	ssize_t r = -1;
	faux_buf_t *faux_buf = NULL;
	char *buf = NULL;
	ssize_t len = 0;
	faux_msg_t *ack = NULL;

	// Some errors or fd is closed so remove it from polling
	if (!(info->revents & POLLIN)) {
		faux_eloop_del_fd(eloop, info->fd);
		return BOOL_TRUE;
	}

	if (!ktpd)
		return BOOL_TRUE;
	if (!ktpd->exec)
		return BOOL_TRUE;

	faux_buf = kexec_bufout(ktpd->exec);
	assert(faux_buf);

	do {
		void *linear_buf = NULL;
		ssize_t really_readed = 0;
		ssize_t linear_len =
			faux_buf_dwrite_lock_easy(faux_buf, &linear_buf);
		// Non-blocked read. The fd became non-blocked while
		// kexec_prepare().
		r = read(info->fd, linear_buf, linear_len);
		if (r > 0)
			really_readed = r;
		faux_buf_dwrite_unlock_easy(faux_buf, really_readed);
	} while (r > 0);

	len = faux_buf_len(faux_buf);
	if (0 == len)
		return BOOL_TRUE;

	buf = malloc(len);
	faux_buf_read(faux_buf, buf, len);

	// Create KTP_STDOUT message to send to client
	ack = ktp_msg_preform(KTP_STDOUT, KTP_STATUS_NONE);
	faux_msg_add_param(ack, KTP_PARAM_LINE, buf, len);
	faux_msg_send_async(ack, ktpd->async);
	faux_msg_free(ack);

	free(buf);

	// Happy compiler
	eloop = eloop;
	type = type;

	return BOOL_TRUE;
}


static bool_t action_stderr_ev(faux_eloop_t *eloop, faux_eloop_type_e type,
	void *associated_data, void *user_data)
{
	faux_eloop_info_fd_t *info = (faux_eloop_info_fd_t *)associated_data;
	ktpd_session_t *ktpd = (ktpd_session_t *)user_data;
	ssize_t r = -1;
	faux_buf_t *faux_buf = NULL;
	char *buf = NULL;
	ssize_t len = 0;
	faux_msg_t *ack = NULL;

	// Some errors or fd is closed so remove it from polling
	if (!(info->revents & POLLIN)) {
		faux_eloop_del_fd(eloop, info->fd);
		return BOOL_TRUE;
	}

	if (!ktpd)
		return BOOL_TRUE;
	if (!ktpd->exec)
		return BOOL_TRUE;

	faux_buf = kexec_buferr(ktpd->exec);
	assert(faux_buf);

	do {
		void *linear_buf = NULL;
		ssize_t really_readed = 0;
		ssize_t linear_len =
			faux_buf_dwrite_lock_easy(faux_buf, &linear_buf);
		// Non-blocked read. The fd became non-blocked while
		// kexec_prepare().
		r = read(info->fd, linear_buf, linear_len);
		if (r > 0)
			really_readed = r;
		faux_buf_dwrite_unlock_easy(faux_buf, really_readed);
	} while (r > 0);

	len = faux_buf_len(faux_buf);
	if (0 == len)
		return BOOL_TRUE;

	buf = malloc(len);
	faux_buf_read(faux_buf, buf, len);

	// Create KTP_STDERR message to send to client
	ack = ktp_msg_preform(KTP_STDERR, KTP_STATUS_NONE);
	faux_msg_add_param(ack, KTP_PARAM_LINE, buf, len);
	faux_msg_send_async(ack, ktpd->async);
	faux_msg_free(ack);

	free(buf);

	// Happy compiler
	eloop = eloop;
	type = type;

	return BOOL_TRUE;
}


bool_t client_ev(faux_eloop_t *eloop, faux_eloop_type_e type,
	void *associated_data, void *user_data)
{
	faux_eloop_info_fd_t *info = (faux_eloop_info_fd_t *)associated_data;
	ktpd_session_t *ktpd = (ktpd_session_t *)user_data;
	faux_async_t *async = ktpd->async;

	assert(async);

	// Write data
	if (info->revents & POLLOUT) {
		faux_eloop_exclude_fd_event(eloop, info->fd, POLLOUT);
		if (faux_async_out(async) < 0) {
			// Someting went wrong
			faux_eloop_del_fd(eloop, info->fd);
			syslog(LOG_ERR, "Problem with async output");
			return BOOL_FALSE; // Stop event loop
		}
	}

	// Read data
	if (info->revents & POLLIN) {
		if (faux_async_in(async) < 0) {
			// Someting went wrong
			faux_eloop_del_fd(eloop, info->fd);
			syslog(LOG_ERR, "Problem with async input");
			return BOOL_FALSE; // Stop event loop
		}
	}

	// EOF
	if (info->revents & POLLHUP) {
		faux_eloop_del_fd(eloop, info->fd);
		syslog(LOG_DEBUG, "Close connection %d", info->fd);
		return BOOL_FALSE; // Stop event loop
	}

	// POLLERR
	if (info->revents & POLLERR) {
		faux_eloop_del_fd(eloop, info->fd);
		syslog(LOG_DEBUG, "POLLERR received %d", info->fd);
		return BOOL_FALSE; // Stop event loop
	}

	// POLLNVAL
	if (info->revents & POLLNVAL) {
		faux_eloop_del_fd(eloop, info->fd);
		syslog(LOG_DEBUG, "POLLNVAL received %d", info->fd);
		return BOOL_FALSE; // Stop event loop
	}

	type = type; // Happy compiler

	// Session can be really finished here. Note KTPD session can't be
	// stopped immediately so it's only two places within code to really
	// break the loop. This one and within wait_for_action_ev().
	if (ktpd->exit)
		return BOOL_FALSE;

	return BOOL_TRUE;
}


#if 0
static void ktpd_session_bad_socket(ktpd_session_t *ktpd)
{
	assert(ktpd);
	if (!ktpd)
		return;

	ktpd->state = KTPD_SESSION_STATE_DISCONNECTED;
}
#endif
