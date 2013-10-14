/* nsrepl.c
 *
 *	Allows one to open a unix socket to access an interpreter running in an
 *	AOLserver instance.
 *
 *	For example, if listenAtPath is /var/run/repl, with socat(1):
 *
 *		socat STDIO /var/run/repl
 *		server1:tcl 1> info tclversion
 *
 *	Because it is a unix-domain socket, it can be secured with standard
 *	file permissions.
 *
 */

/* Newer Linux systems may not get struct ucred unless this is defined!: */
#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <strings.h>
#include <limits.h>

#include <pwd.h>
#include <grp.h>

#include "ns.h"

typedef struct sockaddr_un	unix_sockaddr_t;
#define ASCII_END_OF_TRANSMISSION 4
#define MOD_NAME "nsrepl"

#if defined(LOCAL_PEERCRED) && !defined(SO_PEERCRED)
#	define SO_PEERCRED LOCAL_PEERCRED
#endif

#define TRACE()
#if !defined(TRACE)
#	if defined(__FUNCTION__)
#		define TRACE() do{Ns_Log(Notice, MOD_NAME ": " __FILE__ ":" __FUNCTION__ ":%d", __LINE__);}while(0)
#	elsif define(__FUNC__)
#		define TRACE() do{Ns_Log(Notice, MOD_NAME ": " __FILE__ ":" __FUNC__ ":%d", __LINE__);}while(0)
#	else
#		define TRACE() do{Ns_Log(Notice, MOD_NAME ": " __FILE__ ":%d", __LINE__);}while(0)
#	endif
#endif

#if defined(_POSIX_NAME_MAX) && !defined(LOGIN_NAME_MAX)
#	define LOGIN_NAME_MAX _POSIX_NAME_MAX
#endif

#if defined(SO_PEERCRED) || defined(SCM_CREDENTIALS)
#	define UCRED ucred
	typedef struct ucred who_t;
#endif

/* The NS module settings. */
typedef struct NsREPL {
	char				*server, *listenAtPath;
	int					listenSocket, logCommandsP;
} NsREPL;

static Ns_ThreadProc repl;

/* The following structure is allocated for each session. */
typedef struct REPLSession {
	NsREPL			*nsrepl;
	int				id;

	SOCKET			sock;
	unix_sockaddr_t	addr;
	pid_t			pid;
	uid_t			uid;	char user[LOGIN_NAME_MAX];
	gid_t			gid;	char group[LOGIN_NAME_MAX];

	Tcl_Interp		*interp;
	unsigned int	ncmds;
	unsigned int	nerrs;
} REPLSession;

static Ns_ArgProc ArgProc;
static Ns_SockProc acceptUnixDomainSocket;
static Tcl_CmdProc ExitCmd;
static int ReadLine(SOCKET sock, char *prompt, Tcl_DString *cmd);

const char *EOL_STR = "\n";


/*
 *------------------------------------------------------------------------------
 *
 * NsREPL_ModuleInit --
 *
 *	Load the config parameters, setup the structures, and listen on the unix
 *	socket.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Server will listen for control connections on specified unix socket.
 *
 *------------------------------------------------------------------------------
 */
int
NsREPL_ModuleInit(char *server, char *module) {
	NsREPL	*nsrepl			= ns_malloc(sizeof(NsREPL));
	char	*path			= Ns_ConfigGetPath(server, module, NULL),
			**listenAtPath	= &(nsrepl->listenAtPath);
	int		*listenSocket	= &(nsrepl->listenSocket);
	unix_sockaddr_t name;

	/* Configure the module */
	nsrepl->server			= server;

	*listenAtPath = Ns_ConfigGetValue(path, "listenAtPath");
	if (*listenAtPath == NULL) {
		Ns_DString ds;
		Ns_DStringInit(&ds);
		Ns_DStringVarAppend(&ds, server, ".", module, NULL);
		*listenAtPath = Ns_DStringExport(&ds);
		Ns_Log(Warning, MOD_NAME ": missing listenAtPath parameter, using '%s'.",
			   *listenAtPath);
		Ns_DStringFree(&ds);
	}
	unlink(*listenAtPath);
	memset((char*)&name, 0, sizeof(name));
	name.sun_family = AF_UNIX;
	strncpy(name.sun_path, *listenAtPath, sizeof(name.sun_path));

	if (!Ns_ConfigGetBool(path, "logCommands", &nsrepl->logCommandsP)) {
		nsrepl->logCommandsP = 0; /* Default to off */
	}

	/* Create the listening socket. */
	*listenSocket = socket(AF_UNIX, SOCK_STREAM, 0);
	if (*listenSocket < 0) {
		Ns_Log(Error, MOD_NAME ": could not create socket @ %s", *listenAtPath);
		return NS_ERROR;
	}
	if (bind(*listenSocket, (struct sockaddr *)&name, sizeof(name)) < 0) {
		close(*listenSocket);
		Ns_Log(Error, MOD_NAME ": could not bind to socket @ %s", *listenAtPath);
		return NS_ERROR;
	}
	if (listen(*listenSocket, 16) < 0) {
		close(*listenSocket);
		Ns_Log(Error, MOD_NAME ": could not listen on socket @ %s", *listenAtPath);
		return NS_ERROR;
	}
	Ns_Log(Notice, MOD_NAME ": listening @ %s", *listenAtPath);

	/* set user/group read/write permissions on the socket */
	if (fchmod(*listenSocket, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP) < 0) {
		Ns_Log(Error, MOD_NAME ": could not 'chmod ug=rw' on the socket @ %s: %s",
			   *listenAtPath, strerror(errno));
	}

	/* Register socket callbacks for accepting new connections. */
	Ns_SockCallback(*listenSocket, acceptUnixDomainSocket, nsrepl,
					NS_SOCK_READ|NS_SOCK_EXIT);
	Ns_RegisterProcInfo((void *)acceptUnixDomainSocket, MOD_NAME, ArgProc);
	Ns_Log(Notice, MOD_NAME ": initialized");
	TRACE();
	return NS_OK;
}


static void
ArgProc(Tcl_DString *dsPtr, void *arg)
{
	TRACE();
    Tcl_DStringStartSublist(dsPtr);
    Tcl_DStringEndSublist(dsPtr);
}


/*
 *------------------------------------------------------------------------------
 *
 * acceptUnixDomainSocket --
 *
 *	Ns_Sock* callback to accept a new connection.
 *
 * Results:
 *	NS_TRUE to keep listening unless shutdown is in progress.
 *
 * Side effects:
 *	New repl thread will be created.
 *
 *------------------------------------------------------------------------------
 */
static int
acceptUnixDomainSocket(SOCKET listener, void *nsrepl, int why) {
	TRACE();
	if (why == NS_SOCK_EXIT) {
		Ns_Log(Notice, MOD_NAME ": shutdown");
		ns_sockclose(listener);
		return NS_FALSE;
	}

	REPLSession	*sssn	= ns_malloc(sizeof(REPLSession));
	int			len		= sizeof(struct sockaddr_un);
	sssn->nsrepl		= nsrepl;
	sssn->sock			= Ns_SockAccept(listener, (struct sockaddr*)&sssn->addr, &len);

	if (sssn->sock == INVALID_SOCKET) {
		Ns_Log(Error, MOD_NAME ": accept() failed: %s", ns_sockstrerror(ns_sockerrno));
		ns_free(sssn);
	} else {
		static int next = 0;
		sssn->id = ++next;
		Ns_ThreadCreate(repl, sssn, 0, NULL);
	}
	return NS_TRUE;
}

static REPLSession *initUnixDomainSocket(REPLSession *sssn) {
	TRACE();

#if defined(UCRED)
	who_t			who;
	int				len = sizeof(who);
	if(getsockopt(sssn->sock, SOL_SOCKET, SO_PEERCRED, &who, &len) != 0) {
		Ns_Log(Error, MOD_NAME ": getsockopt() failed: %s", strerror(errno));
		goto no_sockopt;
	} else {
		sssn->pid = who.pid;
		sssn->uid = who.uid;
		sssn->gid = who.gid;
#else
	uid_t	uid;
	gid_t	gid;
	if(getpeereid(sssn->sock, &uid, &gid) != 0) {
		Ns_Log(Error, MOD_NAME ": invalid file descriptor.");
		goto invalid_descriptor;
	}
	sssn->pid = -1;
	sssn->uid = uid;
	sssn->gid = gid;
	{
#endif
		{
			int len = sysconf(_SC_GETPW_R_SIZE_MAX);
			if(len < 1) goto no_pwent_size_max;
			char buf[len];
			struct passwd usr, *pUsr = &usr;
			if(getpwuid_r(sssn->uid, &usr, buf, len, &pUsr) < 0 || pUsr == NULL) {
				goto no_pwent_for_uid;
			}
			strncpy(sssn->user, usr.pw_name, sizeof(sssn->user));
		}
		{
			int len = sysconf(_SC_GETGR_R_SIZE_MAX);
			if(len < 1) goto no_grent_size_max;
			char buf[len];
			struct group grp, *pGrp;
			if(getgrgid_r(sssn->gid, &grp, buf, len, &pGrp) < 0 || pGrp == NULL) {
				goto no_grent_for_gid;
			}
			strncpy(sssn->group, grp.gr_name, sizeof(sssn->group));
		}
	}

	Ns_Log(Notice, MOD_NAME ":%d: connected %s:%s {pid:%d, uid:%d, gid:%d}",
		   sssn->id,
		   sssn->user,	sssn->group,
		   sssn->pid,	sssn->uid, sssn->gid
	);

	return sssn;

 no_pwent_size_max:
 no_pwent_for_uid:
 no_grent_size_max:
 no_grent_for_gid:
#if defined(UCRED)
 no_sockopt:
#else
 invalid_descriptor:
#endif
	close(sssn->sock);
	ns_free(sssn);
	return NULL;
}

static void closeREPL(REPLSession *sssn) {
	TRACE();
	if (sssn->interp != NULL) {
		Ns_TclDeAllocateInterp(sssn->interp);
		sssn->interp = NULL;
	}

	Ns_Log(Notice, MOD_NAME ":%d: disconnected %s:%s {pid:%d, uid:%d, gid:%d}",
		   sssn->id,
		   sssn->user, sssn->group,
		   sssn->pid, sssn->uid, sssn->gid
	);
	ns_sockclose(sssn->sock);
	sssn->sock = -1;
	ns_free(sssn);
}



/*
 *------------------------------------------------------------------------------
 *
 * repl --
 *
 *	Thread to read and evaluate commands from remote.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Depends on commands.
 *
 *------------------------------------------------------------------------------
 */
static void repl(void *replSession) {
	TRACE();
	REPLSession	*sssn		= replSession;
	NsREPL		*nsrepl		= sssn->nsrepl;
	Tcl_Interp	**interp	= &(sssn->interp);
	char		*server		= nsrepl->server;

	if(!initUnixDomainSocket(sssn)) {
		return;
	} else {
		char name[128];
		snprintf(name, sizeof(name),
				 "+" MOD_NAME ":%d:rids{p:%d,u:%d,g:%d}+",
				 sssn->id,
				 sssn->pid,
			 	 sssn->uid,
			 	 sssn->gid
		);
		Ns_ThreadSetName(name);
	}

	/************************************************************************
	 * Loop until the remote shuts down, evaluating complete commands		*
	 ************************************************************************/
	*interp = Ns_TclAllocateInterp(server);

	/* Create a special exit command for this interp only. */
	int stop = 0;
	Tcl_CreateCommand(*interp, "exit", ExitCmd, (ClientData)&stop, NULL);

	int				ncmd = 0, nerr = 0, errCode = TCL_OK;
	char			dynamic_prompt[64];
	char			*prompt			= &dynamic_prompt[0],
					*tcl_prompt1	= Tcl_GetVar(*interp, "tcl_prompt1", TCL_GLOBAL_ONLY),
					*tcl_prompt2	= Tcl_GetVar(*interp, "tcl_prompt2", TCL_GLOBAL_ONLY);

	Tcl_DString		cmd;
	Tcl_DStringInit(&cmd);

	while (!stop) {
		Tcl_DStringTrunc(&cmd, 0);

	start_prompt:
		if((tcl_prompt1 = Tcl_GetVar(*interp, "tcl_prompt1", TCL_GLOBAL_ONLY))) {
			prompt = tcl_prompt1;
		} else {
			snprintf(dynamic_prompt, sizeof(dynamic_prompt), "%s%s:tcl(%d) %d> ",
					 EOL_STR, server, errCode, ncmd);
			prompt = dynamic_prompt;
		}

		/* READ ************************************************************* */
		int n;
		for(n = 0; 1; n++) {
			if (!ReadLine(sssn->sock, prompt, &cmd)) {
				goto no_more_cmds;
			}
			if (Tcl_CommandComplete(cmd.string)) {
				break;
			}
			if(!n) {
				if ((tcl_prompt2 = Tcl_GetVar(*interp, "tcl_prompt2", TCL_GLOBAL_ONLY))) {
					prompt = tcl_prompt2;
				} else {
					snprintf(dynamic_prompt, sizeof(dynamic_prompt), "%s%s:tcl %d\\\t",
							 EOL_STR, server, ncmd);
					prompt = dynamic_prompt;
				}
			}
		}

		/* remove excess trailing line endings */
		int i = cmd.length-1;
		for(; i > 0 && cmd.string[i] != '\n'; i--) /* no-op */;
		Tcl_DStringTrunc(&cmd, i);

		if (STREQ(cmd.string, "")) {
			goto start_prompt; /* Empty command. */
		}

		if (nsrepl->logCommandsP) {
			Ns_Log(Debug, MOD_NAME ": %s %d: start eval %s", sssn->user, ncmd, cmd.string);
		}

		/* EVAL ************************************************************* */
		if ((errCode = Tcl_RecordAndEval(*interp, cmd.string, 0)) != TCL_OK) {
			Ns_TclLogError(*interp);
			sssn->nerrs = ++nerr;
		}
		sssn->ncmds = ++ncmd;

		/* PRINT ************************************************************ */
		char	*res	= Tcl_GetStringResult(*interp);	/*-(*interp)->result;-*/
		int		len		= strlen(res);
		while (len > 0) {
			int n = send(sssn->sock, res, len, 0);
			if (n <= 0) goto no_more_cmds;
			len -= n;
			res += n;
		}

		if (nsrepl->logCommandsP) {
			Ns_Log(Debug, MOD_NAME ": %s %d: end eval", sssn->user, ncmd);
		}
	}

 no_more_cmds:
	send(sssn->sock, EOL_STR, strlen(EOL_STR), 0);
	Tcl_DStringFree(&cmd);
	closeREPL(sssn);
}


/*
 *------------------------------------------------------------------------------
 *
 * ReadLine --
 *
 *	Prompt for a line of input from the remote.  \r\n sequences are translated
 *	to \n.
 *
 * Results:
 *		1 if line received, 0 if remote dropped.
 *
 * Side effects:
 *	Line contents are appended to CMD.
 *
 *------------------------------------------------------------------------------
 */
static int
ReadLine(SOCKET sock, char *prompt, Tcl_DString *cmd) {
	TRACE();
	int n = strlen(prompt);
	if (send(sock, prompt, n, 0) != n) {
		return 0;
	}

	unsigned char buf[2048];
	do {
		if ((n = recv(sock, buf, sizeof(buf), 0)) <= 0) {
			return 0;
		}

		/* Translate CRLF in LF */
		if (n > 1 && buf[n-1] == '\n' && buf[n-2] == '\r') {
			buf[n-2] = '\n';
			--n;
		}

		if (n == 1 && buf[0] == ASCII_END_OF_TRANSMISSION) {
			return 0;
		}
		Tcl_DStringAppend(cmd, buf, n);
	} while (buf[n-1] != '\n');

	return 1;
}


/*
 *------------------------------------------------------------------------------
 *
 * ExitCmd --
 *
 *	Special exit command for the interpreter attached to an nsrepl.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *------------------------------------------------------------------------------
 */
static int
ExitCmd(ClientData stopIntPtr, Tcl_Interp *interp, int argc, CONST char **argv) {
	TRACE();
	if (argc != 1) {
		Tcl_AppendResult(interp, "wrong # args: should be \"",
						 (char*)argv[0], "\"", NULL);
		return TCL_ERROR;
	}
	int *stopPtr = (int *)stopIntPtr;
	*stopPtr = 1;
	Tcl_SetResult(interp, "", TCL_STATIC);
	return TCL_OK;
}
